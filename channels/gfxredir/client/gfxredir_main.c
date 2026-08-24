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
#include <winpr/stream.h>

#include <freerdp/client/channels.h>
#include <freerdp/client/gfxredir.h>
#include <freerdp/channels/log.h>
#include <freerdp/channels/gfxredir.h>

#include "gfxredir_main.h"
#include "gfxredir_common.h"

#define TAG CHANNELS_TAG("gfxredir.client")

typedef struct
{
	GENERIC_DYNVC_PLUGIN base; /* must be the first element */

	GfxRedirClientContext* context;
	UINT32 confirmedCapsVersion;
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

	if (gfxredir->context && gfxredir->context->ClosePool)
		return gfxredir->context->ClosePool(gfxredir->context, &pdu);

	return CHANNEL_RC_OK;
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

	if (gfxredir->context && gfxredir->context->DestroyBuffer)
		return gfxredir->context->DestroyBuffer(gfxredir->context, &pdu);

	return CHANNEL_RC_OK;
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

	WLog_INFO(TAG,
	          "-> PresentBuffer presentId=%" PRIu64 " bufferId=%" PRIu64 " windowId=%" PRIu64
	          " rect=%" PRIu32 "x%" PRIu32 "+%" PRIu32 "+%" PRIu32 " target=%" PRIu32 "x%" PRIu32
	          " orientation=%" PRIu32 " numOpaqueRects=%" PRIu32,
	          pdu.presentId, pdu.bufferId, pdu.windowId, pdu.dirtyRect.width, pdu.dirtyRect.height,
	          pdu.dirtyRect.left, pdu.dirtyRect.top, pdu.targetWidth, pdu.targetHeight,
	          pdu.orientation, pdu.numOpaqueRects);

	if (gfxredir->context && gfxredir->context->PresentBuffer)
		return gfxredir->context->PresentBuffer(gfxredir->context, &pdu);

	return CHANNEL_RC_OK;
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
	Stream_SetPosition(data, beg + header.length);
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
                                       WINPR_ATTR_UNUSED rdpSettings* settings)
{
	GFXREDIR_PLUGIN* gfxredir = (GFXREDIR_PLUGIN*)base;

	WINPR_ASSERT(gfxredir);

	GfxRedirClientContext* context = (GfxRedirClientContext*)calloc(1, sizeof(*context));
	if (!context)
	{
		WLog_Print(base->log, WLOG_ERROR, "unable to allocate GfxRedirClientContext");
		return CHANNEL_RC_NO_MEMORY;
	}

	context->handle = (void*)gfxredir;

	/* Published to the client application as the ChannelConnected pInterface. */
	gfxredir->base.iface.pInterface = gfxredir->context = context;

	return CHANNEL_RC_OK;
}

static void gfxredir_plugin_terminated(GENERIC_DYNVC_PLUGIN* base)
{
	GFXREDIR_PLUGIN* gfxredir = (GFXREDIR_PLUGIN*)base;

	WINPR_ASSERT(gfxredir);

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
