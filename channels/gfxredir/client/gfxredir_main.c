/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDPXXXX Remote App Graphics Redirection Virtual Channel Extension
 *
 * Client side implementation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/memory.h>
#include <winpr/stream.h>
#include <winpr/string.h>

#include <freerdp/settings.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/gfxredir.h>
#include <freerdp/channels/log.h>
#include <freerdp/channels/gfxredir.h>

#include "gfxredir_main.h"
#include "gfxredir_common.h"

#define TAG CHANNELS_TAG("gfxredir.client")

/* The server only ever needs a handful; a linear scan is cheaper than a map. */
#define GFXREDIR_MAX_BUFFERS 8

typedef struct
{
	BOOL used;
	GFXREDIR_CREATE_BUFFER_PDU desc;
} GFXREDIR_BUFFER;

typedef struct
{
	GENERIC_DYNVC_PLUGIN base; /* must be the first element */

	GfxRedirClientContext* context;
	UINT32 confirmedCapsVersion;

	/* Namespace prefix the server's pool section names are relative to, from
	 * /wslgsharedmemorypath. Without it we cannot map a pool at all. */
	char* sharedMemoryPath;

	/* The single pool the server has open, if any. Mapped but not yet used for
	 * anything -- presentation still has to be built on top. */
	UINT64 poolId;
	HANDLE poolMapping;
	void* poolAddr;
	UINT64 poolSize;

	GFXREDIR_BUFFER buffers[GFXREDIR_MAX_BUFFERS];
} GFXREDIR_PLUGIN;

static GFXREDIR_PLUGIN* gfxredir_get_plugin(GENERIC_CHANNEL_CALLBACK* callback)
{
	WINPR_ASSERT(callback);
	return (GFXREDIR_PLUGIN*)callback->plugin;
}

/**
 * Allocate a stream holding a single PDU with its GFXREDIR_HEADER filled in.
 *
 * @return the new stream or nullptr on failure
 */
static wStream* gfxredir_packet_new(UINT32 cmdId, size_t length)
{
	GFXREDIR_HEADER header = { 0 };
	wStream* s = Stream_New(nullptr, GFXREDIR_HEADER_SIZE + length);

	if (!s)
	{
		WLog_ERR(TAG, "Stream_New failed!");
		return nullptr;
	}

	header.cmdId = cmdId;
	header.length = (UINT32)(GFXREDIR_HEADER_SIZE + length);

	if (gfxredir_write_header(s, &header) != CHANNEL_RC_OK)
	{
		Stream_Free(s, TRUE);
		return nullptr;
	}

	return s;
}

/**
 * Write the stream out on the DVC and free it.
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_packet_send(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	UINT ret = ERROR_INTERNAL_ERROR;

	WINPR_ASSERT(callback);
	WINPR_ASSERT(callback->channel);
	WINPR_ASSERT(callback->channel->Write);

	const size_t pos = Stream_GetPosition(s);
	if (pos <= UINT32_MAX)
		ret = callback->channel->Write(callback->channel, (ULONG)pos, Stream_Buffer(s), nullptr);

	Stream_Free(s, TRUE);
	return ret;
}

/**
 * Advertise the capability sets this client supports. Sent as soon as the
 * server opens the channel; the server answers with a CAPS_CONFIRM naming the
 * single version it selected.
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_send_caps_advertise(GENERIC_CHANNEL_CALLBACK* callback)
{
	wStream* s = gfxredir_packet_new(GFXREDIR_CMDID_CAPS_ADVERTISE,
	                                 GFXREDIR_CAPS_HEADER_SIZE + 4 /* supportedFeatures */);

	if (!s)
		return CHANNEL_RC_NO_MEMORY;

	/* A single GFXREDIR_CAPS_V2_0_PDU. */
	Stream_Write_UINT32(s, GFXREDIR_CAPS_SIGNATURE);
	Stream_Write_UINT32(s, GFXREDIR_CAPS_VERSION2_0);
	Stream_Write_UINT32(s, GFXREDIR_CAPS_HEADER_SIZE + 4);
	Stream_Write_UINT32(s, 0); /* supportedFeatures, reserved */

	WLog_INFO(TAG, "<- CapsAdvertise version=0x%x", GFXREDIR_CAPS_VERSION2_0);
	return gfxredir_packet_send(callback, s);
}

/**
 * Tell the server we are done with a presented buffer. Until this arrives the
 * server keeps exactly one present outstanding and sends nothing further, so a
 * missing ack stalls the whole stream.
 *
 * Exposed on the client context so an application doing asynchronous
 * presentation can ack on its own schedule; see deferPresentBufferAck.
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_ack_present(GENERIC_CHANNEL_CALLBACK* callback, UINT64 windowId,
                                 UINT64 presentId)
{
	wStream* s = gfxredir_packet_new(GFXREDIR_CMDID_PRESENT_BUFFER_ACK, 16);

	if (!s)
		return CHANNEL_RC_NO_MEMORY;

	Stream_Write_UINT64(s, windowId);
	Stream_Write_UINT64(s, presentId);

	WLog_DBG(TAG, "<- PresentBufferAck presentId=%" PRIu64 " windowId=%" PRIu64, presentId,
	         windowId);
	return gfxredir_packet_send(callback, s);
}

/* Context entry point for applications that ack on their own schedule. */
static UINT gfxredir_send_present_buffer_ack(GfxRedirClientContext* context,
                                             const GFXREDIR_PRESENT_BUFFER_ACK_PDU* ack)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(ack);

	GFXREDIR_PLUGIN* gfxredir = (GFXREDIR_PLUGIN*)context->handle;
	WINPR_ASSERT(gfxredir);

	GENERIC_LISTENER_CALLBACK* listener = gfxredir->base.listener_callback;
	if (!listener || !listener->channel_callback)
	{
		WLog_ERR(TAG, "cannot ack present, channel is not open");
		return ERROR_INTERNAL_ERROR;
	}

	return gfxredir_ack_present(listener->channel_callback, ack->windowId, ack->presentId);
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_recv_caps_confirm(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	GFXREDIR_PLUGIN* gfxredir = gfxredir_get_plugin(callback);
	UINT32 signature = 0;
	UINT32 version = 0;
	UINT32 length = 0;

	if (!Stream_CheckAndLogRequiredLength(TAG, s, GFXREDIR_CAPS_HEADER_SIZE))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT32(s, signature);
	Stream_Read_UINT32(s, version);
	Stream_Read_UINT32(s, length);

	WLog_INFO(TAG, "-> CapsConfirm version=0x%" PRIx32 " length=%" PRIu32, version, length);

	if (signature != GFXREDIR_CAPS_SIGNATURE)
	{
		WLog_ERR(TAG, "caps confirm with invalid signature 0x%08" PRIx32, signature);
		return ERROR_INVALID_DATA;
	}

	if (version != GFXREDIR_CAPS_VERSION2_0)
	{
		WLog_ERR(TAG, "server confirmed unsupported caps version 0x%" PRIx32, version);
		return ERROR_INVALID_DATA;
	}

	gfxredir->confirmedCapsVersion = version;
	WLog_INFO(TAG, "gfxredir negotiated, caps version 0x%" PRIx32, version);

	if (gfxredir->context && gfxredir->context->CapsConfirm)
		return gfxredir->context->CapsConfirm(gfxredir->context, version);

	return CHANNEL_RC_OK;
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_recv_error(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	GFXREDIR_PLUGIN* gfxredir = gfxredir_get_plugin(callback);
	GFXREDIR_ERROR_PDU pdu = { 0 };

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 4))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT32(s, pdu.errorCode);
	WLog_ERR(TAG, "-> Error errorCode=0x%08" PRIx32, pdu.errorCode);

	if (gfxredir->context && gfxredir->context->Error)
		return gfxredir->context->Error(gfxredir->context, &pdu);

	return CHANNEL_RC_OK;
}

static void gfxredir_track_buffer(GFXREDIR_PLUGIN* gfxredir,
                                  const GFXREDIR_CREATE_BUFFER_PDU* pdu)
{
	GFXREDIR_BUFFER* slot = nullptr;

	/* A repeated bufferId replaces the old description. */
	for (size_t i = 0; i < GFXREDIR_MAX_BUFFERS; i++)
	{
		if (gfxredir->buffers[i].used && (gfxredir->buffers[i].desc.bufferId == pdu->bufferId))
		{
			slot = &gfxredir->buffers[i];
			break;
		}
	}

	if (!slot)
	{
		for (size_t i = 0; i < GFXREDIR_MAX_BUFFERS; i++)
		{
			if (!gfxredir->buffers[i].used)
			{
				slot = &gfxredir->buffers[i];
				break;
			}
		}
	}

	if (!slot)
	{
		WLog_ERR(TAG, "no free buffer slot for bufferId=%" PRIu64, pdu->bufferId);
		return;
	}

	slot->used = TRUE;
	slot->desc = *pdu;
}

static void gfxredir_forget_buffer(GFXREDIR_PLUGIN* gfxredir, UINT64 bufferId)
{
	for (size_t i = 0; i < GFXREDIR_MAX_BUFFERS; i++)
	{
		if (gfxredir->buffers[i].used && (gfxredir->buffers[i].desc.bufferId == bufferId))
			gfxredir->buffers[i].used = FALSE;
	}
}

static void gfxredir_forget_pool_buffers(GFXREDIR_PLUGIN* gfxredir, UINT64 poolId)
{
	for (size_t i = 0; i < GFXREDIR_MAX_BUFFERS; i++)
	{
		if (gfxredir->buffers[i].used && (gfxredir->buffers[i].desc.poolId == poolId))
			gfxredir->buffers[i].used = FALSE;
	}
}

/**
 * Resolve a buffer to a pointer into the mapped pool, validating that the
 * geometry the server described actually fits inside the mapping. The server
 * is trusted but not blindly: a bad offset/stride/height would otherwise read
 * off the end of the section.
 */
static BOOL gfxredir_get_buffer_mapping(GfxRedirClientContext* context, UINT64 bufferId,
                                        GFXREDIR_BUFFER_MAPPING* mapping)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(mapping);

	GFXREDIR_PLUGIN* gfxredir = (GFXREDIR_PLUGIN*)context->handle;
	WINPR_ASSERT(gfxredir);

	const GFXREDIR_BUFFER* buffer = nullptr;
	for (size_t i = 0; i < GFXREDIR_MAX_BUFFERS; i++)
	{
		if (gfxredir->buffers[i].used && (gfxredir->buffers[i].desc.bufferId == bufferId))
		{
			buffer = &gfxredir->buffers[i];
			break;
		}
	}

	if (!buffer)
	{
		WLog_ERR(TAG, "unknown bufferId=%" PRIu64, bufferId);
		return FALSE;
	}

	const GFXREDIR_CREATE_BUFFER_PDU* desc = &buffer->desc;

	if (!gfxredir->poolAddr || (gfxredir->poolId != desc->poolId))
	{
		WLog_ERR(TAG, "pool %" PRIu64 " for bufferId=%" PRIu64 " is not mapped", desc->poolId,
		         bufferId);
		return FALSE;
	}

	if ((desc->height == 0) || (desc->width == 0) || (desc->stride < (4ull * desc->width)))
	{
		WLog_ERR(TAG, "bufferId=%" PRIu64 " has bad geometry %" PRIu32 "x%" PRIu32 " stride=%" PRIu32,
		         bufferId, desc->width, desc->height, desc->stride);
		return FALSE;
	}

	/* Last row only needs width pixels, not a full stride. */
	const UINT64 span = ((UINT64)(desc->height - 1) * desc->stride) + (4ull * desc->width);
	if ((desc->offset > gfxredir->poolSize) || (span > (gfxredir->poolSize - desc->offset)))
	{
		WLog_ERR(TAG,
		         "bufferId=%" PRIu64 " does not fit its pool: offset=%" PRIu64 " span=%" PRIu64
		         " poolSize=%" PRIu64,
		         bufferId, desc->offset, span, gfxredir->poolSize);
		return FALSE;
	}

	mapping->data = (const BYTE*)gfxredir->poolAddr + desc->offset;
	mapping->size = (size_t)span;
	mapping->stride = desc->stride;
	mapping->width = desc->width;
	mapping->height = desc->height;
	mapping->format = desc->format;
	return TRUE;
}

/* Release whatever pool mapping we are holding, if any. */
static void gfxredir_unmap_pool(GFXREDIR_PLUGIN* gfxredir)
{
	if (gfxredir->poolAddr)
	{
		(void)UnmapViewOfFile(gfxredir->poolAddr);
		gfxredir->poolAddr = nullptr;
	}

	if (gfxredir->poolMapping)
	{
		(void)CloseHandle(gfxredir->poolMapping);
		gfxredir->poolMapping = nullptr;
	}

	gfxredir->poolId = 0;
	gfxredir->poolSize = 0;
}

/**
 * Map the shared memory section backing a pool. The server sends only the
 * section's own name; it lives under the namespace passed on the command line
 * as /wslgsharedmemorypath.
 *
 * Nothing reads the mapping yet -- this only proves we can reach the memory.
 */
static void gfxredir_map_pool(GFXREDIR_PLUGIN* gfxredir, const GFXREDIR_OPEN_POOL_PDU* pdu)
{
	char* sectionName = nullptr;
	char* fullName = nullptr;

	if (!gfxredir->sharedMemoryPath)
	{
		WLog_WARN(TAG, "no /wslgsharedmemorypath given, cannot map pool %" PRIu64, pdu->poolId);
		return;
	}

	gfxredir_unmap_pool(gfxredir);

	/* sectionNameLength counts wchars and includes the terminator. */
	sectionName = ConvertWCharNToUtf8Alloc((const WCHAR*)pdu->sectionName,
	                                       pdu->sectionNameLength, nullptr);
	if (!sectionName)
	{
		WLog_ERR(TAG, "failed to convert pool section name");
		return;
	}

	size_t fullNameLen = 0;
	if (winpr_asprintf(&fullName, &fullNameLen, "%s\\%s", gfxredir->sharedMemoryPath,
	                   sectionName) < 0)
	{
		fullName = nullptr;
		goto out;
	}

	gfxredir->poolMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, fullName);
	if (!gfxredir->poolMapping)
	{
		WLog_ERR(TAG, "OpenFileMapping(\"%s\") failed with 0x%08" PRIx32, fullName,
		         (UINT32)GetLastError());
		goto out;
	}

	if (pdu->poolSize > SIZE_MAX)
	{
		WLog_ERR(TAG, "pool size %" PRIu64 " does not fit in this address space", pdu->poolSize);
		gfxredir_unmap_pool(gfxredir);
		goto out;
	}

	gfxredir->poolAddr = MapViewOfFile(gfxredir->poolMapping, FILE_MAP_READ, 0, 0,
	                                   (SIZE_T)pdu->poolSize);
	if (!gfxredir->poolAddr)
	{
		WLog_ERR(TAG, "MapViewOfFile(\"%s\", %" PRIu64 ") failed with 0x%08" PRIx32, fullName,
		         pdu->poolSize, (UINT32)GetLastError());
		gfxredir_unmap_pool(gfxredir);
		goto out;
	}

	gfxredir->poolId = pdu->poolId;
	gfxredir->poolSize = pdu->poolSize;
	WLog_INFO(TAG, "mapped pool %" PRIu64 " \"%s\" (%" PRIu64 " bytes) at %p", pdu->poolId,
	          fullName, pdu->poolSize, gfxredir->poolAddr);

out:
	free(sectionName);
	free(fullName);
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_recv_open_pool(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	GFXREDIR_PLUGIN* gfxredir = gfxredir_get_plugin(callback);
	GFXREDIR_OPEN_POOL_PDU pdu = { 0 };

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 20))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT64(s, pdu.poolId);
	Stream_Read_UINT64(s, pdu.poolSize);
	Stream_Read_UINT32(s, pdu.sectionNameLength);

	/* sectionName is a Windows-style 2-byte wchar string, null terminated. */
	if (!Stream_CheckAndLogRequiredLengthOfSize(TAG, s, pdu.sectionNameLength, 2ull))
		return ERROR_INVALID_DATA;

	pdu.sectionName = (const unsigned short*)Stream_ConstPointer(s);
	Stream_Seek(s, 2ull * pdu.sectionNameLength);

	WLog_INFO(TAG,
	          "-> OpenPool poolId=%" PRIu64 " poolSize=%" PRIu64 " sectionNameLength=%" PRIu32,
	          pdu.poolId, pdu.poolSize, pdu.sectionNameLength);

	gfxredir_map_pool(gfxredir, &pdu);

	if (gfxredir->context && gfxredir->context->OpenPool)
		return gfxredir->context->OpenPool(gfxredir->context, &pdu);

	return CHANNEL_RC_OK;
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_recv_close_pool(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	GFXREDIR_PLUGIN* gfxredir = gfxredir_get_plugin(callback);
	GFXREDIR_CLOSE_POOL_PDU pdu = { 0 };

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT64(s, pdu.poolId);
	WLog_INFO(TAG, "-> ClosePool poolId=%" PRIu64, pdu.poolId);

	/* Notify before unmapping: the application may be holding a view of this
	 * memory and needs the chance to drop it first. */
	UINT error = CHANNEL_RC_OK;
	if (gfxredir->context && gfxredir->context->ClosePool)
		error = gfxredir->context->ClosePool(gfxredir->context, &pdu);

	gfxredir_forget_pool_buffers(gfxredir, pdu.poolId);
	if (gfxredir->poolId == pdu.poolId)
		gfxredir_unmap_pool(gfxredir);

	return error;
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_recv_create_buffer(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	GFXREDIR_PLUGIN* gfxredir = gfxredir_get_plugin(callback);
	GFXREDIR_CREATE_BUFFER_PDU pdu = { 0 };

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 40))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT64(s, pdu.poolId);
	Stream_Read_UINT64(s, pdu.bufferId);
	Stream_Read_UINT64(s, pdu.offset);
	Stream_Read_UINT32(s, pdu.stride);
	Stream_Read_UINT32(s, pdu.width);
	Stream_Read_UINT32(s, pdu.height);
	Stream_Read_UINT32(s, pdu.format);

	WLog_INFO(TAG,
	          "-> CreateBuffer bufferId=%" PRIu64 " poolId=%" PRIu64 " %" PRIu32 "x%" PRIu32
	          " stride=%" PRIu32 " offset=%" PRIu64 " format=%" PRIu32,
	          pdu.bufferId, pdu.poolId, pdu.width, pdu.height, pdu.stride, pdu.offset, pdu.format);

	gfxredir_track_buffer(gfxredir, &pdu);

	if (gfxredir->context && gfxredir->context->CreateBuffer)
		return gfxredir->context->CreateBuffer(gfxredir->context, &pdu);

	return CHANNEL_RC_OK;
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_recv_destroy_buffer(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	GFXREDIR_PLUGIN* gfxredir = gfxredir_get_plugin(callback);
	GFXREDIR_DESTROY_BUFFER_PDU pdu = { 0 };

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 8))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT64(s, pdu.bufferId);
	WLog_INFO(TAG, "-> DestroyBuffer bufferId=%" PRIu64, pdu.bufferId);

	/* Notify first, then forget: see gfxredir_recv_close_pool. */
	UINT error = CHANNEL_RC_OK;
	if (gfxredir->context && gfxredir->context->DestroyBuffer)
		error = gfxredir->context->DestroyBuffer(gfxredir->context, &pdu);

	gfxredir_forget_buffer(gfxredir, pdu.bufferId);

	return error;
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_recv_present_buffer(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)
{
	GFXREDIR_PLUGIN* gfxredir = gfxredir_get_plugin(callback);
	GFXREDIR_PRESENT_BUFFER_PDU pdu = { 0 };
	RECTANGLE_32 opaqueRects[GFXREDIR_MAX_OPAQUE_RECTS] = { 0 };

	if (!Stream_CheckAndLogRequiredLength(TAG, s, 64))
		return ERROR_INVALID_DATA;

	Stream_Read_UINT64(s, pdu.timestamp);
	Stream_Read_UINT64(s, pdu.presentId);
	Stream_Read_UINT64(s, pdu.windowId);
	Stream_Read_UINT64(s, pdu.bufferId);
	Stream_Read_UINT32(s, pdu.orientation);
	Stream_Read_UINT32(s, pdu.targetWidth);
	Stream_Read_UINT32(s, pdu.targetHeight);
	Stream_Read_UINT32(s, pdu.dirtyRect.left);
	Stream_Read_UINT32(s, pdu.dirtyRect.top);
	Stream_Read_UINT32(s, pdu.dirtyRect.width);
	Stream_Read_UINT32(s, pdu.dirtyRect.height);
	Stream_Read_UINT32(s, pdu.numOpaqueRects);

	if (pdu.numOpaqueRects > GFXREDIR_MAX_OPAQUE_RECTS)
	{
		WLog_ERR(TAG, "numOpaqueRects %" PRIu32 " exceeds the limit", pdu.numOpaqueRects);
		return ERROR_INVALID_DATA;
	}

	if (!Stream_CheckAndLogRequiredLengthOfSize(TAG, s, pdu.numOpaqueRects, 16ull))
		return ERROR_INVALID_DATA;

	for (UINT32 i = 0; i < pdu.numOpaqueRects; i++)
	{
		Stream_Read_UINT32(s, opaqueRects[i].left);
		Stream_Read_UINT32(s, opaqueRects[i].top);
		Stream_Read_UINT32(s, opaqueRects[i].width);
		Stream_Read_UINT32(s, opaqueRects[i].height);
	}
	pdu.opaqueRects = opaqueRects;

	WLog_DBG(TAG,
	         "-> PresentBuffer presentId=%" PRIu64 " bufferId=%" PRIu64 " windowId=%" PRIu64
	         " rect=%" PRIu32 "x%" PRIu32 "+%" PRIu32 "+%" PRIu32 " target=%" PRIu32 "x%" PRIu32
	         " orientation=%" PRIu32 " numOpaqueRects=%" PRIu32,
	         pdu.presentId, pdu.bufferId, pdu.windowId, pdu.dirtyRect.width, pdu.dirtyRect.height,
	         pdu.dirtyRect.left, pdu.dirtyRect.top, pdu.targetWidth, pdu.targetHeight,
	         pdu.orientation, pdu.numOpaqueRects);

	if (gfxredir->context && gfxredir->context->PresentBuffer)
	{
		const UINT error = gfxredir->context->PresentBuffer(gfxredir->context, &pdu);
		if (error)
			return error;
	}

	/* The server will not send another present until this is acked. An
	 * application that keeps using the buffer past the callback opts out and
	 * acks for itself. */
	if (gfxredir->context && gfxredir->context->deferPresentBufferAck)
		return CHANNEL_RC_OK;

	return gfxredir_ack_present(callback, pdu.windowId, pdu.presentId);
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	GFXREDIR_HEADER header = { 0 };
	UINT error = CHANNEL_RC_OK;

	WINPR_ASSERT(callback);

	const size_t beg = Stream_GetPosition(data);

	if ((error = gfxredir_read_header(data, &header)))
		return error;

	if ((header.length < GFXREDIR_HEADER_SIZE) ||
	    !Stream_CheckAndLogRequiredLength(TAG, data, header.length - GFXREDIR_HEADER_SIZE))
		return ERROR_INVALID_DATA;

	switch (header.cmdId)
	{
		case GFXREDIR_CMDID_CAPS_CONFIRM:
			error = gfxredir_recv_caps_confirm(callback, data);
			break;

		case GFXREDIR_CMDID_ERROR:
			error = gfxredir_recv_error(callback, data);
			break;

		case GFXREDIR_CMDID_OPEN_POOL:
			error = gfxredir_recv_open_pool(callback, data);
			break;

		case GFXREDIR_CMDID_CLOSE_POOL:
			error = gfxredir_recv_close_pool(callback, data);
			break;

		case GFXREDIR_CMDID_CREATE_BUFFER:
			error = gfxredir_recv_create_buffer(callback, data);
			break;

		case GFXREDIR_CMDID_DESTROY_BUFFER:
			error = gfxredir_recv_destroy_buffer(callback, data);
			break;

		case GFXREDIR_CMDID_PRESENT_BUFFER:
			error = gfxredir_recv_present_buffer(callback, data);
			break;

		default:
			WLog_WARN(TAG, "-> unknown PDU cmdId=0x%08" PRIx32 " length=%" PRIu32, header.cmdId,
			          header.length);
			break;
	}

	if (error)
	{
		WLog_ERR(TAG, "PDU cmdId=0x%08" PRIx32 " failed with error %" PRIu32, header.cmdId, error);
		return error;
	}

	/* Skip whatever the handler did not consume, PDUs are packed back to back. */
	if (!Stream_SetPosition(data, beg + header.length))
		return ERROR_INVALID_DATA;

	return CHANNEL_RC_OK;
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_on_open(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;

	WLog_INFO(TAG, "channel opened");
	return gfxredir_send_caps_advertise(callback);
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_on_close(IWTSVirtualChannelCallback* pChannelCallback)
{
	WLog_INFO(TAG, "channel closed");
	free(pChannelCallback);
	return CHANNEL_RC_OK;
}

/**
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gfxredir_plugin_initialize(GENERIC_DYNVC_PLUGIN* base,
                                       WINPR_ATTR_UNUSED rdpContext* rcontext,
                                       rdpSettings* settings)
{
	GFXREDIR_PLUGIN* gfxredir = (GFXREDIR_PLUGIN*)base;

	WINPR_ASSERT(gfxredir);

	/* /wslgsharedmemorypath is passed through as this channel's addin argument;
	 * see the command line handler in client/common/cmdline.c. */
	const ADDIN_ARGV* args = freerdp_dynamic_channel_collection_find(settings, "gfxredir");
	if (args && (args->argc > 1) && args->argv[1])
	{
		gfxredir->sharedMemoryPath = _strdup(args->argv[1]);
		if (!gfxredir->sharedMemoryPath)
			return CHANNEL_RC_NO_MEMORY;

		WLog_INFO(TAG, "shared memory path \"%s\"", gfxredir->sharedMemoryPath);
	}
	else
	{
		WLog_WARN(TAG, "no /wslgsharedmemorypath given, pools cannot be mapped");
	}

	GfxRedirClientContext* context = (GfxRedirClientContext*)calloc(1, sizeof(*context));
	if (!context)
	{
		WLog_Print(base->log, WLOG_ERROR, "unable to allocate GfxRedirClientContext");
		return CHANNEL_RC_NO_MEMORY;
	}

	context->handle = (void*)gfxredir;
	context->PresentBufferAck = gfxredir_send_present_buffer_ack;
	context->GetBufferMapping = gfxredir_get_buffer_mapping;

	/* Published to the client application as the ChannelConnected pInterface. */
	gfxredir->base.iface.pInterface = gfxredir->context = context;

	return CHANNEL_RC_OK;
}

static void gfxredir_plugin_terminated(GENERIC_DYNVC_PLUGIN* base)
{
	GFXREDIR_PLUGIN* gfxredir = (GFXREDIR_PLUGIN*)base;

	WINPR_ASSERT(gfxredir);

	gfxredir_unmap_pool(gfxredir);

	free(gfxredir->sharedMemoryPath);
	gfxredir->sharedMemoryPath = nullptr;

	free(gfxredir->context);
	gfxredir->context = nullptr;
}

static const IWTSVirtualChannelCallback gfxredir_callbacks = { gfxredir_on_data_received,
	                                                           gfxredir_on_open,
	                                                           gfxredir_on_close, nullptr };

FREERDP_ENTRY_POINT(UINT VCAPITYPE gfxredir_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))
{
	return freerdp_generic_DVCPluginEntry(
	    pEntryPoints, TAG, GFXREDIR_DVC_CHANNEL_NAME, sizeof(GFXREDIR_PLUGIN),
	    sizeof(GENERIC_CHANNEL_CALLBACK), &gfxredir_callbacks, gfxredir_plugin_initialize,
	    gfxredir_plugin_terminated);
}
