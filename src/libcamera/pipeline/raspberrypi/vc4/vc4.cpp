/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019-2023, Raspberry Pi Ltd
 *
 * vc4.cpp - Pipeline handler for VC4 based Raspberry Pi devices
 */

#include <linux/bcm2835-isp.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include <libcamera/formats.h>

#include "libcamera/internal/device_enumerator.h"

#include "dma_heaps.h"
#include "pipeline_base.h"
#include "rpi_stream.h"

using namespace std::chrono_literals;

namespace libcamera {

LOG_DECLARE_CATEGORY(RPI)

using StreamFlags = RPi::Stream::Flags;

namespace {

enum class Unicam : unsigned int { Image, Embedded };
enum class Isp : unsigned int { Input, Output0, Output1, Stats };

} /* namespace */

class Vc4CameraData final : public RPi::CameraData
{
public:
	Vc4CameraData(PipelineHandler *pipe)
		: RPi::CameraData(pipe)
	{
	}

	~Vc4CameraData()
	{
		freeBuffers();
	}

	V4L2VideoDevice::Formats ispFormats() const override
	{
		return isp_[Isp::Output0].dev()->formats();
	}

	V4L2VideoDevice::Formats rawFormats() const override
	{
		return unicam_[Unicam::Image].dev()->formats();
	}

	V4L2VideoDevice *frontendDevice() override
	{
		return unicam_[Unicam::Image].dev();
	}

	void platformFreeBuffers() override
	{
	}

	CameraConfiguration::Status platformValidate(std::vector<StreamParams> &rawStreams,
						     std::vector<StreamParams> &outStreams) const override;

	int platformPipelineConfigure(const std::unique_ptr<YamlObject> &root) override;

	void platformStart() override;
	void platformStop() override;

	void unicamBufferDequeue(FrameBuffer *buffer);
	void ispInputDequeue(FrameBuffer *buffer);
	void ispOutputDequeue(FrameBuffer *buffer);

	void processStatsComplete(const ipa::RPi::BufferIds &buffers);
	void prepareIspComplete(const ipa::RPi::BufferIds &buffers);
	void setIspControls(const ControlList &controls);
	void setCameraTimeout(uint32_t maxFrameLengthMs);

	/* Array of Unicam and ISP device streams and associated buffers/streams. */
	RPi::Device<Unicam, 2> unicam_;
	RPi::Device<Isp, 4> isp_;

	/* DMAHEAP allocation helper. */
	RPi::DmaHeap dmaHeap_;
	SharedFD lsTable_;

	struct Config {
		/*
		 * The minimum number of internal buffers to be allocated for
		 * the Unicam Image stream.
		 */
		unsigned int minUnicamBuffers;
		/*
		 * The minimum total (internal + external) buffer count used for
		 * the Unicam Image stream.
		 *
		 * Note that:
		 * minTotalUnicamBuffers must be >= 1, and
		 * minTotalUnicamBuffers >= minUnicamBuffers
		 */
		unsigned int minTotalUnicamBuffers;
	};

	Config config_;

private:
	void platformIspCrop() override
	{
		isp_[Isp::Input].dev()->setSelection(V4L2_SEL_TGT_CROP, &ispCrop_);
	}

	int platformConfigure(const V4L2SubdeviceFormat &sensorFormat,
			      std::optional<BayerFormat::Packing> packing,
			      std::vector<StreamParams> &rawStreams,
			      std::vector<StreamParams> &outStreams) override;
	int platformConfigureIpa(ipa::RPi::ConfigParams &params) override;

	int platformInitIpa([[maybe_unused]] ipa::RPi::InitParams &params) override
	{
		return 0;
	}

	struct BayerFrame {
		FrameBuffer *buffer;
		ControlList controls;
		unsigned int delayContext;
	};

	void tryRunPipeline() override;
	bool findMatchingBuffers(BayerFrame &bayerFrame, FrameBuffer *&embeddedBuffer);

	std::queue<BayerFrame> bayerQueue_;
	std::queue<FrameBuffer *> embeddedQueue_;
};

class PipelineHandlerVc4 : public RPi::PipelineHandlerBase
{
public:
	PipelineHandlerVc4(CameraManager *manager)
		: RPi::PipelineHandlerBase(manager)
	{
	}

	~PipelineHandlerVc4()
	{
	}

	bool match(DeviceEnumerator *enumerator) override;

private:
	Vc4CameraData *cameraData(Camera *camera)
	{
		return static_cast<Vc4CameraData *>(camera->_d());
	}

	std::unique_ptr<RPi::CameraData> allocateCameraData()
	{
		return std::make_unique<Vc4CameraData>(this);
	}

	int prepareBuffers(Camera *camera) override;
	int platformRegister(std::unique_ptr<RPi::CameraData> &cameraData, MediaDevice *unicam, MediaDevice *isp);
};

bool PipelineHandlerVc4::match(DeviceEnumerator *enumerator)
{
	constexpr unsigned int numUnicamDevices = 2;

	/*
	 * Loop over all Unicam instances, but return out once a match is found.
	 * This is to ensure we correctly enumrate the camera when an instance
	 * of Unicam has registered with media controller, but has not registered
	 * device nodes due to a sensor subdevice failure.
	 */
	for (unsigned int i = 0; i < numUnicamDevices; i++) {
		DeviceMatch unicam("unicam");
		MediaDevice *unicamDevice = acquireMediaDevice(enumerator, unicam);

		if (!unicamDevice) {
			LOG(RPI, Debug) << "Unable to acquire a Unicam instance";
			break;
		}

		DeviceMatch isp("bcm2835-isp");
		MediaDevice *ispDevice = acquireMediaDevice(enumerator, isp);

		if (!ispDevice) {
			LOG(RPI, Debug) << "Unable to acquire ISP instance";
			break;
		}

		/*
		 * The loop below is used to register multiple cameras behind one or more
		 * video mux devices that are attached to a particular Unicam instance.
		 * Obviously these cameras cannot be used simultaneously.
		 */
		unsigned int numCameras = 0;
		for (MediaEntity *entity : unicamDevice->entities()) {
			if (entity->function() != MEDIA_ENT_F_CAM_SENSOR)
				continue;

			int ret = RPi::PipelineHandlerBase::registerCamera(unicamDevice, "unicam-image",
									   ispDevice, entity);
			if (ret)
				LOG(RPI, Error) << "Failed to register camera "
						<< entity->name() << ": " << ret;
			else
				numCameras++;
		}

		if (numCameras)
			return true;
	}

	return false;
}

int PipelineHandlerVc4::prepareBuffers(Camera *camera)
{
	Vc4CameraData *data = cameraData(camera);
	unsigned int numRawBuffers = 0;
	int ret;

	for (Stream *s : camera->streams()) {
		if (BayerFormat::fromPixelFormat(s->configuration().pixelFormat).isValid()) {
			numRawBuffers = s->configuration().bufferCount;
			break;
		}
	}

	/* Decide how many internal buffers to allocate. */
	for (auto const stream : data->streams_) {
		unsigned int numBuffers;
		/*
		 * For Unicam, allocate a minimum number of buffers for internal
		 * use as we want to avoid any frame drops.
		 */
		const unsigned int minBuffers = data->config_.minTotalUnicamBuffers;
		if (stream == &data->unicam_[Unicam::Image]) {
			/*
			 * If an application has configured a RAW stream, allocate
			 * additional buffers to make up the minimum, but ensure
			 * we have at least minUnicamBuffers of internal buffers
			 * to use to minimise frame drops.
			 */
			numBuffers = std::max<int>(data->config_.minUnicamBuffers,
						   minBuffers - numRawBuffers);
		} else if (stream == &data->isp_[Isp::Input]) {
			/*
			 * ISP input buffers are imported from Unicam, so follow
			 * similar logic as above to count all the RAW buffers
			 * available.
			 */
			numBuffers = numRawBuffers +
				     std::max<int>(data->config_.minUnicamBuffers,
						   minBuffers - numRawBuffers);

		} else if (stream == &data->unicam_[Unicam::Embedded]) {
			/*
			 * Embedded data buffers are (currently) for internal use,
			 * so allocate the minimum required to avoid frame drops.
			 */
			numBuffers = minBuffers;
		} else {
			/*
			 * Since the ISP runs synchronous with the IPA and requests,
			 * we only ever need one set of internal buffers. Any buffers
			 * the application wants to hold onto will already be exported
			 * through PipelineHandlerRPi::exportFrameBuffers().
			 */
			numBuffers = 1;
		}

		ret = stream->prepareBuffers(numBuffers);
		if (ret < 0)
			return ret;
	}

	/*
	 * Pass the stats and embedded data buffers to the IPA. No other
	 * buffers need to be passed.
	 */
	mapBuffers(camera, data->isp_[Isp::Stats].getBuffers(), RPi::MaskStats);
	if (data->sensorMetadata_)
		mapBuffers(camera, data->unicam_[Unicam::Embedded].getBuffers(),
			   RPi::MaskEmbeddedData);

	return 0;
}

int PipelineHandlerVc4::platformRegister(std::unique_ptr<RPi::CameraData> &cameraData, MediaDevice *unicam, MediaDevice *isp)
{
	Vc4CameraData *data = static_cast<Vc4CameraData *>(cameraData.get());

	if (!data->dmaHeap_.isValid())
		return -ENOMEM;

	MediaEntity *unicamImage = unicam->getEntityByName("unicam-image");
	MediaEntity *ispOutput0 = isp->getEntityByName("bcm2835-isp0-output0");
	MediaEntity *ispCapture1 = isp->getEntityByName("bcm2835-isp0-capture1");
	MediaEntity *ispCapture2 = isp->getEntityByName("bcm2835-isp0-capture2");
	MediaEntity *ispCapture3 = isp->getEntityByName("bcm2835-isp0-capture3");

	if (!unicamImage || !ispOutput0 || !ispCapture1 || !ispCapture2 || !ispCapture3)
		return -ENOENT;

	/* Locate and open the unicam video streams. */
	data->unicam_[Unicam::Image] = RPi::Stream("Unicam Image", unicamImage);

	/* An embedded data node will not be present if the sensor does not support it. */
	MediaEntity *unicamEmbedded = unicam->getEntityByName("unicam-embedded");
	if (unicamEmbedded) {
		data->unicam_[Unicam::Embedded] = RPi::Stream("Unicam Embedded", unicamEmbedded);
		data->unicam_[Unicam::Embedded].dev()->bufferReady.connect(data,
									   &Vc4CameraData::unicamBufferDequeue);
	}

	/* Tag the ISP input stream as an import stream. */
	data->isp_[Isp::Input] = RPi::Stream("ISP Input", ispOutput0, StreamFlags::ImportOnly);
	data->isp_[Isp::Output0] = RPi::Stream("ISP Output0", ispCapture1);
	data->isp_[Isp::Output1] = RPi::Stream("ISP Output1", ispCapture2);
	data->isp_[Isp::Stats] = RPi::Stream("ISP Stats", ispCapture3);

	/* Wire up all the buffer connections. */
	data->unicam_[Unicam::Image].dev()->bufferReady.connect(data, &Vc4CameraData::unicamBufferDequeue);
	data->isp_[Isp::Input].dev()->bufferReady.connect(data, &Vc4CameraData::ispInputDequeue);
	data->isp_[Isp::Output0].dev()->bufferReady.connect(data, &Vc4CameraData::ispOutputDequeue);
	data->isp_[Isp::Output1].dev()->bufferReady.connect(data, &Vc4CameraData::ispOutputDequeue);
	data->isp_[Isp::Stats].dev()->bufferReady.connect(data, &Vc4CameraData::ispOutputDequeue);

	if (data->sensorMetadata_ ^ !!data->unicam_[Unicam::Embedded].dev()) {
		LOG(RPI, Warning) << "Mismatch between Unicam and CamHelper for embedded data usage!";
		data->sensorMetadata_ = false;
		if (data->unicam_[Unicam::Embedded].dev())
			data->unicam_[Unicam::Embedded].dev()->bufferReady.disconnect();
	}

	/*
	 * Open all Unicam and ISP streams. The exception is the embedded data
	 * stream, which only gets opened below if the IPA reports that the sensor
	 * supports embedded data.
	 *
	 * The below grouping is just for convenience so that we can easily
	 * iterate over all streams in one go.
	 */
	data->streams_.push_back(&data->unicam_[Unicam::Image]);
	if (data->sensorMetadata_)
		data->streams_.push_back(&data->unicam_[Unicam::Embedded]);

	for (auto &stream : data->isp_)
		data->streams_.push_back(&stream);

	for (auto stream : data->streams_) {
		int ret = stream->dev()->open();
		if (ret)
			return ret;
	}

	if (!data->unicam_[Unicam::Image].dev()->caps().hasMediaController()) {
		LOG(RPI, Error) << "Unicam driver does not use the MediaController, please update your kernel!";
		return -EINVAL;
	}

	/* Write up all the IPA connections. */
	data->ipa_->processStatsComplete.connect(data, &Vc4CameraData::processStatsComplete);
	data->ipa_->prepareIspComplete.connect(data, &Vc4CameraData::prepareIspComplete);
	data->ipa_->setIspControls.connect(data, &Vc4CameraData::setIspControls);
	data->ipa_->setCameraTimeout.connect(data, &Vc4CameraData::setCameraTimeout);

	/*
	 * List the available streams an application may request. At present, we
	 * do not advertise Unicam Embedded and ISP Statistics streams, as there
	 * is no mechanism for the application to request non-image buffer formats.
	 */
	std::set<Stream *> streams;
	streams.insert(&data->unicam_[Unicam::Image]);
	streams.insert(&data->isp_[Isp::Output0]);
	streams.insert(&data->isp_[Isp::Output1]);

	/* Create and register the camera. */
	const std::string &id = data->sensor_->id();
	std::shared_ptr<Camera> camera =
		Camera::create(std::move(cameraData), id, streams);
	PipelineHandler::registerCamera(std::move(camera));

	LOG(RPI, Info) << "Registered camera " << id
		       << " to Unicam device " << unicam->deviceNode()
		       << " and ISP device " << isp->deviceNode();

	return 0;
}

CameraConfiguration::Status Vc4CameraData::platformValidate(std::vector<StreamParams> &rawStreams,
							    std::vector<StreamParams> &outStreams) const
{
	CameraConfiguration::Status status = CameraConfiguration::Status::Valid;

	/* Can only output 1 RAW stream, or 2 YUV/RGB streams. */
	if (rawStreams.size() > 1 || outStreams.size() > 2) {
		LOG(RPI, Error) << "Invalid number of streams requested";
		return CameraConfiguration::Status::Invalid;
	}

	if (!rawStreams.empty())
		rawStreams[0].dev = unicam_[Unicam::Image].dev();

	/*
	 * For the two ISP outputs, one stream must be equal or smaller than the
	 * other in all dimensions.
	 *
	 * Index 0 contains the largest requested resolution.
	 */
	for (unsigned int i = 0; i < outStreams.size(); i++) {
		Size size;

		size.width = std::min(outStreams[i].cfg->size.width,
				      outStreams[0].cfg->size.width);
		size.height = std::min(outStreams[i].cfg->size.height,
				       outStreams[0].cfg->size.height);

		if (outStreams[i].cfg->size != size) {
			outStreams[i].cfg->size = size;
			status = CameraConfiguration::Status::Adjusted;
		}

		/*
		 * Output 0 must be for the largest resolution. We will
		 * have that fixed up in the code above.
		 */
		outStreams[i].dev = isp_[i == 0 ? Isp::Output0 : Isp::Output1].dev();
	}

	return status;
}

int Vc4CameraData::platformPipelineConfigure(const std::unique_ptr<YamlObject> &root)
{
	config_ = {
		.minUnicamBuffers = 2,
		.minTotalUnicamBuffers = 4,
	};

	if (!root)
		return 0;

	std::optional<double> ver = (*root)["version"].get<double>();
	if (!ver || *ver != 1.0) {
		LOG(RPI, Error) << "Unexpected configuration file version reported";
		return -EINVAL;
	}

	std::optional<std::string> target = (*root)["target"].get<std::string>();
	if (!target || *target != "bcm2835") {
		LOG(RPI, Error) << "Unexpected target reported: expected \"bcm2835\", got "
				<< *target;
		return -EINVAL;
	}

	const YamlObject &phConfig = (*root)["pipeline_handler"];
	config_.minUnicamBuffers =
		phConfig["min_unicam_buffers"].get<unsigned int>(config_.minUnicamBuffers);
	config_.minTotalUnicamBuffers =
		phConfig["min_total_unicam_buffers"].get<unsigned int>(config_.minTotalUnicamBuffers);

	if (config_.minTotalUnicamBuffers < config_.minUnicamBuffers) {
		LOG(RPI, Error) << "Invalid configuration: min_total_unicam_buffers must be >= min_unicam_buffers";
		return -EINVAL;
	}

	if (config_.minTotalUnicamBuffers < 1) {
		LOG(RPI, Error) << "Invalid configuration: min_total_unicam_buffers must be >= 1";
		return -EINVAL;
	}

	return 0;
}

int Vc4CameraData::platformConfigure(const V4L2SubdeviceFormat &sensorFormat,
				     std::optional<BayerFormat::Packing> packing,
				     std::vector<StreamParams> &rawStreams,
				     std::vector<StreamParams> &outStreams)
{
	int ret;

	if (!packing)
		packing = BayerFormat::Packing::CSI2;

	V4L2VideoDevice *unicam = unicam_[Unicam::Image].dev();
	V4L2DeviceFormat unicamFormat = RPi::PipelineHandlerBase::toV4L2DeviceFormat(unicam, sensorFormat, *packing);

	ret = unicam->setFormat(&unicamFormat);
	if (ret)
		return ret;

	/*
	 * See which streams are requested, and route the user
	 * StreamConfiguration appropriately.
	 */
	if (!rawStreams.empty()) {
		rawStreams[0].cfg->setStream(&unicam_[Unicam::Image]);
		unicam_[Unicam::Image].setFlags(StreamFlags::External);
	}

	ret = isp_[Isp::Input].dev()->setFormat(&unicamFormat);
	if (ret)
		return ret;

	LOG(RPI, Info) << "Sensor: " << sensor_->id()
		       << " - Selected sensor format: " << sensorFormat
		       << " - Selected unicam format: " << unicamFormat;

	/* Use a sensible small default size if no output streams are configured. */
	Size maxSize = outStreams.empty() ? Size(320, 240) : outStreams[0].cfg->size;
	V4L2DeviceFormat format;

	for (unsigned int i = 0; i < outStreams.size(); i++) {
		StreamConfiguration *cfg = outStreams[i].cfg;

		/* The largest resolution gets routed to the ISP Output 0 node. */
		RPi::Stream *stream = i == 0 ? &isp_[Isp::Output0] : &isp_[Isp::Output1];

		V4L2PixelFormat fourcc = stream->dev()->toV4L2PixelFormat(cfg->pixelFormat);
		format.size = cfg->size;
		format.fourcc = fourcc;
		format.colorSpace = cfg->colorSpace;

		LOG(RPI, Debug) << "Setting " << stream->name() << " to "
				<< format;

		ret = stream->dev()->setFormat(&format);
		if (ret)
			return -EINVAL;

		if (format.size != cfg->size || format.fourcc != fourcc) {
			LOG(RPI, Error)
				<< "Failed to set requested format on " << stream->name()
				<< ", returned " << format;
			return -EINVAL;
		}

		LOG(RPI, Debug)
			<< "Stream " << stream->name() << " has color space "
			<< ColorSpace::toString(cfg->colorSpace);

		cfg->setStream(stream);
		stream->setFlags(StreamFlags::External);
	}

	ispOutputTotal_ = outStreams.size();

	/*
	 * If ISP::Output0 stream has not been configured by the application,
	 * we must allow the hardware to generate an output so that the data
	 * flow in the pipeline handler remains consistent, and we still generate
	 * statistics for the IPA to use. So enable the output at a very low
	 * resolution for internal use.
	 *
	 * \todo Allow the pipeline to work correctly without Output0 and only
	 * statistics coming from the hardware.
	 */
	if (outStreams.empty()) {
		V4L2VideoDevice *dev = isp_[Isp::Output0].dev();

		format = {};
		format.size = maxSize;
		format.fourcc = dev->toV4L2PixelFormat(formats::YUV420);
		/* No one asked for output, so the color space doesn't matter. */
		format.colorSpace = ColorSpace::Sycc;
		ret = dev->setFormat(&format);
		if (ret) {
			LOG(RPI, Error)
				<< "Failed to set default format on ISP Output0: "
				<< ret;
			return -EINVAL;
		}

		ispOutputTotal_++;

		LOG(RPI, Debug) << "Defaulting ISP Output0 format to "
				<< format;
	}

	/*
	 * If ISP::Output1 stream has not been requested by the application, we
	 * set it up for internal use now. This second stream will be used for
	 * fast colour denoise, and must be a quarter resolution of the ISP::Output0
	 * stream. However, also limit the maximum size to 1200 pixels in the
	 * larger dimension, just to avoid being wasteful with buffer allocations
	 * and memory bandwidth.
	 *
	 * \todo If Output 1 format is not YUV420, Output 1 ought to be disabled as
	 * colour denoise will not run.
	 */
	if (outStreams.size() == 1) {
		V4L2VideoDevice *dev = isp_[Isp::Output1].dev();

		V4L2DeviceFormat output1Format;
		constexpr Size maxDimensions(1200, 1200);
		const Size limit = maxDimensions.boundedToAspectRatio(format.size);

		output1Format.size = (format.size / 2).boundedTo(limit).alignedDownTo(2, 2);
		output1Format.colorSpace = format.colorSpace;
		output1Format.fourcc = dev->toV4L2PixelFormat(formats::YUV420);

		LOG(RPI, Debug) << "Setting ISP Output1 (internal) to "
				<< output1Format;

		ret = dev->setFormat(&output1Format);
		if (ret) {
			LOG(RPI, Error) << "Failed to set format on ISP Output1: "
					<< ret;
			return -EINVAL;
		}

		ispOutputTotal_++;
	}

	/* ISP statistics output format. */
	format = {};
	format.fourcc = V4L2PixelFormat(V4L2_META_FMT_BCM2835_ISP_STATS);
	ret = isp_[Isp::Stats].dev()->setFormat(&format);
	if (ret) {
		LOG(RPI, Error) << "Failed to set format on ISP stats stream: "
				<< format;
		return ret;
	}

	ispOutputTotal_++;

	/*
	 * Configure the Unicam embedded data output format only if the sensor
	 * supports it.
	 */
	if (sensorMetadata_) {
		V4L2SubdeviceFormat embeddedFormat;

		sensor_->device()->getFormat(1, &embeddedFormat);
		format = {};
		format.fourcc = V4L2PixelFormat(V4L2_META_FMT_SENSOR_DATA);
		format.planes[0].size = embeddedFormat.size.width * embeddedFormat.size.height;

		LOG(RPI, Debug) << "Setting embedded data format " << format.toString();
		ret = unicam_[Unicam::Embedded].dev()->setFormat(&format);
		if (ret) {
			LOG(RPI, Error) << "Failed to set format on Unicam embedded: "
					<< format;
			return ret;
		}
	}

	/* Figure out the smallest selection the ISP will allow. */
	Rectangle testCrop(0, 0, 1, 1);
	isp_[Isp::Input].dev()->setSelection(V4L2_SEL_TGT_CROP, &testCrop);
	ispMinCropSize_ = testCrop.size();

	/* Adjust aspect ratio by providing crops on the input image. */
	Size size = unicamFormat.size.boundedToAspectRatio(maxSize);
	ispCrop_ = size.centeredTo(Rectangle(unicamFormat.size).center());

	platformIspCrop();

	return 0;
}

int Vc4CameraData::platformConfigureIpa(ipa::RPi::ConfigParams &params)
{
	params.ispControls = isp_[Isp::Input].dev()->controls();

	/* Allocate the lens shading table via dmaHeap and pass to the IPA. */
	if (!lsTable_.isValid()) {
		lsTable_ = SharedFD(dmaHeap_.alloc("ls_grid", ipa::RPi::MaxLsGridSize));
		if (!lsTable_.isValid())
			return -ENOMEM;

		/* Allow the IPA to mmap the LS table via the file descriptor. */
		/*
		 * \todo Investigate if mapping the lens shading table buffer
		 * could be handled with mapBuffers().
		 */
		params.lsTableHandle = lsTable_;
	}

	return 0;
}

void Vc4CameraData::platformStart()
{
}

void Vc4CameraData::platformStop()
{
	bayerQueue_ = {};
	embeddedQueue_ = {};
}

void Vc4CameraData::unicamBufferDequeue(FrameBuffer *buffer)
{
	RPi::Stream *stream = nullptr;
	int index;

	if (!isRunning())
		return;

	for (RPi::Stream &s : unicam_) {
		index = s.getBufferId(buffer);
		if (index) {
			stream = &s;
			break;
		}
	}

	/* The buffer must belong to one of our streams. */
	ASSERT(stream);

	LOG(RPI, Debug) << "Stream " << stream->name() << " buffer dequeue"
			<< ", buffer id " << index
			<< ", timestamp: " << buffer->metadata().timestamp;

	if (stream == &unicam_[Unicam::Image]) {
		/*
		 * Lookup the sensor controls used for this frame sequence from
		 * DelayedControl and queue them along with the frame buffer.
		 */
		auto [ctrl, delayContext] = delayedCtrls_->get(buffer->metadata().sequence);
		/*
		 * Add the frame timestamp to the ControlList for the IPA to use
		 * as it does not receive the FrameBuffer object.
		 */
		ctrl.set(controls::SensorTimestamp, buffer->metadata().timestamp);
		bayerQueue_.push({ buffer, std::move(ctrl), delayContext });
	} else {
		embeddedQueue_.push(buffer);
	}

	handleState();
}

void Vc4CameraData::ispInputDequeue(FrameBuffer *buffer)
{
	if (!isRunning())
		return;

	LOG(RPI, Debug) << "Stream ISP Input buffer complete"
			<< ", buffer id " << unicam_[Unicam::Image].getBufferId(buffer)
			<< ", timestamp: " << buffer->metadata().timestamp;

	/* The ISP input buffer gets re-queued into Unicam. */
	handleStreamBuffer(buffer, &unicam_[Unicam::Image]);
	handleState();
}

void Vc4CameraData::ispOutputDequeue(FrameBuffer *buffer)
{
	RPi::Stream *stream = nullptr;
	int index;

	if (!isRunning())
		return;

	for (RPi::Stream &s : isp_) {
		index = s.getBufferId(buffer);
		if (index) {
			stream = &s;
			break;
		}
	}

	/* The buffer must belong to one of our ISP output streams. */
	ASSERT(stream);

	LOG(RPI, Debug) << "Stream " << stream->name() << " buffer complete"
			<< ", buffer id " << index
			<< ", timestamp: " << buffer->metadata().timestamp;

	/*
	 * ISP statistics buffer must not be re-queued or sent back to the
	 * application until after the IPA signals so.
	 */
	if (stream == &isp_[Isp::Stats]) {
		ipa::RPi::ProcessParams params;
		params.buffers.stats = index | RPi::MaskStats;
		params.ipaContext = requestQueue_.front()->sequence();
		ipa_->signalProcessStats(params);
	} else {
		/* Any other ISP output can be handed back to the application now. */
		handleStreamBuffer(buffer, stream);
	}

	/*
	 * Increment the number of ISP outputs generated.
	 * This is needed to track dropped frames.
	 */
	ispOutputCount_++;

	handleState();
}

void Vc4CameraData::processStatsComplete(const ipa::RPi::BufferIds &buffers)
{
	if (!isRunning())
		return;

	FrameBuffer *buffer = isp_[Isp::Stats].getBuffers().at(buffers.stats & RPi::MaskID).buffer;

	handleStreamBuffer(buffer, &isp_[Isp::Stats]);

	/* Last thing to do is to fill up the request metadata. */
	Request *request = requestQueue_.front();
	ControlList metadata;

	ipa_->reportMetadata(request->sequence(), &metadata);
	request->metadata().merge(metadata);

	/*
	 * Inform the sensor of the latest colour gains if it has the
	 * V4L2_CID_NOTIFY_GAINS control (which means notifyGainsUnity_ is set).
	 */
	const auto &colourGains = metadata.get(libcamera::controls::ColourGains);
	if (notifyGainsUnity_ && colourGains) {
		/* The control wants linear gains in the order B, Gb, Gr, R. */
		ControlList ctrls(sensor_->controls());
		std::array<int32_t, 4> gains{
			static_cast<int32_t>((*colourGains)[1] * *notifyGainsUnity_),
			*notifyGainsUnity_,
			*notifyGainsUnity_,
			static_cast<int32_t>((*colourGains)[0] * *notifyGainsUnity_)
		};
		ctrls.set(V4L2_CID_NOTIFY_GAINS, Span<const int32_t>{ gains });

		sensor_->setControls(&ctrls);
	}

	state_ = State::IpaComplete;
	handleState();
}

void Vc4CameraData::prepareIspComplete(const ipa::RPi::BufferIds &buffers)
{
	unsigned int embeddedId = buffers.embedded & RPi::MaskID;
	unsigned int bayer = buffers.bayer & RPi::MaskID;
	FrameBuffer *buffer;

	if (!isRunning())
		return;

	buffer = unicam_[Unicam::Image].getBuffers().at(bayer & RPi::MaskID).buffer;
	LOG(RPI, Debug) << "Input re-queue to ISP, buffer id " << (bayer & RPi::MaskID)
			<< ", timestamp: " << buffer->metadata().timestamp;

	isp_[Isp::Input].queueBuffer(buffer);
	ispOutputCount_ = 0;

	if (sensorMetadata_ && embeddedId) {
		buffer = unicam_[Unicam::Embedded].getBuffers().at(embeddedId & RPi::MaskID).buffer;
		handleStreamBuffer(buffer, &unicam_[Unicam::Embedded]);
	}

	handleState();
}

void Vc4CameraData::setIspControls(const ControlList &controls)
{
	ControlList ctrls = controls;

	if (ctrls.contains(V4L2_CID_USER_BCM2835_ISP_LENS_SHADING)) {
		ControlValue &value =
			const_cast<ControlValue &>(ctrls.get(V4L2_CID_USER_BCM2835_ISP_LENS_SHADING));
		Span<uint8_t> s = value.data();
		bcm2835_isp_lens_shading *ls =
			reinterpret_cast<bcm2835_isp_lens_shading *>(s.data());
		ls->dmabuf = lsTable_.get();
	}

	isp_[Isp::Input].dev()->setControls(&ctrls);
	handleState();
}

void Vc4CameraData::setCameraTimeout(uint32_t maxFrameLengthMs)
{
	/*
	 * Set the dequeue timeout to the larger of 5x the maximum reported
	 * frame length advertised by the IPA over a number of frames. Allow
	 * a minimum timeout value of 1s.
	 */
	utils::Duration timeout =
		std::max<utils::Duration>(1s, 5 * maxFrameLengthMs * 1ms);

	LOG(RPI, Debug) << "Setting Unicam timeout to " << timeout;
	unicam_[Unicam::Image].dev()->setDequeueTimeout(timeout);
}

void Vc4CameraData::tryRunPipeline()
{
	FrameBuffer *embeddedBuffer;
	BayerFrame bayerFrame;

	/* If any of our request or buffer queues are empty, we cannot proceed. */
	if (state_ != State::Idle || requestQueue_.empty() ||
	    bayerQueue_.empty() || (embeddedQueue_.empty() && sensorMetadata_))
		return;

	if (!findMatchingBuffers(bayerFrame, embeddedBuffer))
		return;

	/* Take the first request from the queue and action the IPA. */
	Request *request = requestQueue_.front();

	/* See if a new ScalerCrop value needs to be applied. */
	calculateScalerCrop(request->controls());

	/*
	 * Clear the request metadata and fill it with some initial non-IPA
	 * related controls. We clear it first because the request metadata
	 * may have been populated if we have dropped the previous frame.
	 */
	request->metadata().clear();
	fillRequestMetadata(bayerFrame.controls, request);

	/* Set our state to say the pipeline is active. */
	state_ = State::Busy;

	unsigned int bayer = unicam_[Unicam::Image].getBufferId(bayerFrame.buffer);

	LOG(RPI, Debug) << "Signalling signalIspPrepare:"
			<< " Bayer buffer id: " << bayer;

	ipa::RPi::PrepareParams params;
	params.buffers.bayer = RPi::MaskBayerData | bayer;
	params.sensorControls = std::move(bayerFrame.controls);
	params.requestControls = request->controls();
	params.ipaContext = request->sequence();
	params.delayContext = bayerFrame.delayContext;

	if (embeddedBuffer) {
		unsigned int embeddedId = unicam_[Unicam::Embedded].getBufferId(embeddedBuffer);

		params.buffers.embedded = RPi::MaskEmbeddedData | embeddedId;
		LOG(RPI, Debug) << "Signalling signalPrepareIsp:"
				<< " Embedded buffer id: " << embeddedId;
	}

	ipa_->signalPrepareIsp(params);
}

bool Vc4CameraData::findMatchingBuffers(BayerFrame &bayerFrame, FrameBuffer *&embeddedBuffer)
{
	if (bayerQueue_.empty())
		return false;

	/*
	 * Find the embedded data buffer with a matching timestamp to pass to
	 * the IPA. Any embedded buffers with a timestamp lower than the
	 * current bayer buffer will be removed and re-queued to the driver.
	 */
	uint64_t ts = bayerQueue_.front().buffer->metadata().timestamp;
	embeddedBuffer = nullptr;
	while (!embeddedQueue_.empty()) {
		FrameBuffer *b = embeddedQueue_.front();
		if (b->metadata().timestamp < ts) {
			embeddedQueue_.pop();
			unicam_[Unicam::Embedded].returnBuffer(b);
			LOG(RPI, Debug) << "Dropping unmatched input frame in stream "
					<< unicam_[Unicam::Embedded].name();
		} else if (b->metadata().timestamp == ts) {
			/* Found a match! */
			embeddedBuffer = b;
			embeddedQueue_.pop();
			break;
		} else {
			break; /* Only higher timestamps from here. */
		}
	}

	if (!embeddedBuffer && sensorMetadata_) {
		if (embeddedQueue_.empty()) {
			/*
			 * If the embedded buffer queue is empty, wait for the next
			 * buffer to arrive - dequeue ordering may send the image
			 * buffer first.
			 */
			LOG(RPI, Debug) << "Waiting for next embedded buffer.";
			return false;
		}

		/* Log if there is no matching embedded data buffer found. */
		LOG(RPI, Debug) << "Returning bayer frame without a matching embedded buffer.";
	}

	bayerFrame = std::move(bayerQueue_.front());
	bayerQueue_.pop();

	return true;
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerVc4)

} /* namespace libcamera */