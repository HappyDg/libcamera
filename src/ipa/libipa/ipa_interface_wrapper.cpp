/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * ipa_interface_wrapper.cpp - Image Processing Algorithm interface wrapper
 */

#include "ipa_interface_wrapper.h"

#include <map>
#include <string.h>
#include <unistd.h>
#include <vector>

#include <ipa/ipa_interface.h>

#include "byte_stream_buffer.h"

/**
 * \file ipa_interface_wrapper.h
 * \brief Image Processing Algorithm interface wrapper
 */

namespace libcamera {

/**
 * \class IPAInterfaceWrapper
 * \brief Wrap an IPAInterface and expose it as an ipa_context
 *
 * This class implements the ipa_context API based on a provided IPAInterface.
 * It helps IPAs that implement the IPAInterface API to provide the external
 * ipa_context API.
 *
 * To use the wrapper, an IPA module simple creates a new instance of its
 * IPAInterface implementation, and passes it to the constructor of the
 * IPAInterfaceWrapper. As IPAInterfaceWrapper inherits from ipa_context, the
 * constructed wrapper can then be directly returned from the IPA module's
 * ipaCreate() function.
 *
 * \code{.cpp}
 * class MyIPA : public IPAInterface
 * {
 * 	...
 * };
 *
 * struct ipa_context *ipaCreate()
 * {
 * 	return new IPAInterfaceWrapper(utils::make_unique<MyIPA>());
 * }
 * \endcode
 *
 * The wrapper takes ownership of the IPAInterface and will automatically
 * delete it when the wrapper is destroyed.
 */

/**
 * \brief Construct an IPAInterfaceWrapper wrapping \a interface
 * \param[in] interface The interface to wrap
 */
IPAInterfaceWrapper::IPAInterfaceWrapper(std::unique_ptr<IPAInterface> interface)
	: ipa_(std::move(interface)), callbacks_(nullptr), cb_ctx_(nullptr)
{
	ops = &operations_;

	ipa_->queueFrameAction.connect(this, &IPAInterfaceWrapper::queueFrameAction);
}

void IPAInterfaceWrapper::destroy(struct ipa_context *_ctx)
{
	IPAInterfaceWrapper *ctx = static_cast<IPAInterfaceWrapper *>(_ctx);

	delete ctx;
}

void IPAInterfaceWrapper::init(struct ipa_context *_ctx)
{
	IPAInterfaceWrapper *ctx = static_cast<IPAInterfaceWrapper *>(_ctx);

	ctx->ipa_->init();
}

void IPAInterfaceWrapper::register_callbacks(struct ipa_context *_ctx,
					     const struct ipa_callback_ops *callbacks,
					     void *cb_ctx)
{
	IPAInterfaceWrapper *ctx = static_cast<IPAInterfaceWrapper *>(_ctx);

	ctx->callbacks_ = callbacks;
	ctx->cb_ctx_ = cb_ctx;
}

void IPAInterfaceWrapper::configure(struct ipa_context *_ctx,
				    const struct ipa_stream *streams,
				    unsigned int num_streams,
				    const struct ipa_control_info_map *maps,
				    unsigned int num_maps)
{
	IPAInterfaceWrapper *ctx = static_cast<IPAInterfaceWrapper *>(_ctx);

	ctx->serializer_.reset();

	/* Translate the IPA stream configurations map. */
	std::map<unsigned int, IPAStream> ipaStreams;

	for (unsigned int i = 0; i < num_streams; ++i) {
		const struct ipa_stream &stream = streams[i];

		ipaStreams[stream.id] = {
			stream.pixel_format,
			Size(stream.width, stream.height),
		};
	}

	/* Translate the IPA entity controls map. */
	std::map<unsigned int, const ControlInfoMap &> entityControls;
	std::map<unsigned int, ControlInfoMap> infoMaps;

	for (unsigned int i = 0; i < num_maps; ++i) {
		const struct ipa_control_info_map &ipa_map = maps[i];
		ByteStreamBuffer byteStream(ipa_map.data, ipa_map.size);
		unsigned int id = ipa_map.id;

		infoMaps[id] = ctx->serializer_.deserialize<ControlInfoMap>(byteStream);
		entityControls.emplace(id, infoMaps[id]);
	}

	ctx->ipa_->configure(ipaStreams, entityControls);
}

void IPAInterfaceWrapper::map_buffers(struct ipa_context *_ctx,
				      const struct ipa_buffer *_buffers,
				      size_t num_buffers)
{
	IPAInterfaceWrapper *ctx = static_cast<IPAInterfaceWrapper *>(_ctx);
	std::vector<IPABuffer> buffers(num_buffers);

	for (unsigned int i = 0; i < num_buffers; ++i) {
		const struct ipa_buffer &_buffer = _buffers[i];
		IPABuffer &buffer = buffers[i];
		std::vector<Plane> &planes = buffer.memory.planes();

		buffer.id = _buffer.id;

		planes.resize(_buffer.num_planes);
		for (unsigned int j = 0; j < _buffer.num_planes; ++j) {
			if (_buffer.planes[j].dmabuf != -1)
				planes[j].setDmabuf(_buffer.planes[j].dmabuf,
						    _buffer.planes[j].length);
			/** \todo Create a Dmabuf class to implement RAII. */
			::close(_buffer.planes[j].dmabuf);
		}
	}

	ctx->ipa_->mapBuffers(buffers);
}

void IPAInterfaceWrapper::unmap_buffers(struct ipa_context *_ctx,
					const unsigned int *_ids,
					size_t num_buffers)
{
	IPAInterfaceWrapper *ctx = static_cast<IPAInterfaceWrapper *>(_ctx);
	std::vector<unsigned int> ids(_ids, _ids + num_buffers);
	ctx->ipa_->unmapBuffers(ids);
}

void IPAInterfaceWrapper::process_event(struct ipa_context *_ctx,
					const struct ipa_operation_data *data)
{
	IPAInterfaceWrapper *ctx = static_cast<IPAInterfaceWrapper *>(_ctx);
	IPAOperationData opData;

	opData.operation = data->operation;

	opData.data.resize(data->num_data);
	memcpy(opData.data.data(), data->data,
	       data->num_data * sizeof(*data->data));

	opData.controls.resize(data->num_lists);
	for (unsigned int i = 0; i < data->num_lists; ++i) {
		const struct ipa_control_list *c_list = &data->lists[i];
		ByteStreamBuffer byteStream(c_list->data, c_list->size);
		opData.controls[i] = ctx->serializer_.deserialize<ControlList>(byteStream);
	}

	ctx->ipa_->processEvent(opData);
}

void IPAInterfaceWrapper::queueFrameAction(unsigned int frame,
					   const IPAOperationData &data)
{
	if (!callbacks_)
		return;

	struct ipa_operation_data c_data;
	c_data.operation = data.operation;
	c_data.data = data.data.data();
	c_data.num_data = data.data.size();

	struct ipa_control_list control_lists[data.controls.size()];
	c_data.lists = control_lists;
	c_data.num_lists = data.controls.size();

	std::size_t listsSize = 0;
	for (const auto &list : data.controls)
		listsSize += serializer_.binarySize(list);

	std::vector<uint8_t> binaryData(listsSize);
	ByteStreamBuffer byteStreamBuffer(binaryData.data(), listsSize);

	unsigned int i = 0;
	for (const auto &list : data.controls) {
		struct ipa_control_list &c_list = control_lists[i];
		c_list.size = serializer_.binarySize(list);

		ByteStreamBuffer b = byteStreamBuffer.carveOut(c_list.size);
		serializer_.serialize(list, b);

		c_list.data = b.base();
	}

	callbacks_->queue_frame_action(cb_ctx_, frame, c_data);
}

#ifndef __DOXYGEN__
/*
 * This construct confuses Doygen and makes it believe that all members of the
 * operations is a member of IPAInterfaceWrapper. It must thus be hidden.
 */
const struct ipa_context_ops IPAInterfaceWrapper::operations_ = {
	.destroy = &IPAInterfaceWrapper::destroy,
	.init = &IPAInterfaceWrapper::init,
	.register_callbacks = &IPAInterfaceWrapper::register_callbacks,
	.configure = &IPAInterfaceWrapper::configure,
	.map_buffers = &IPAInterfaceWrapper::map_buffers,
	.unmap_buffers = &IPAInterfaceWrapper::unmap_buffers,
	.process_event = &IPAInterfaceWrapper::process_event,
};
#endif

} /* namespace libcamera */
