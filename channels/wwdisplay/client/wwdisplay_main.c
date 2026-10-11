/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Weaselway display information channel, see freerdp/channels/wwdisplay.h
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

#include <winpr/assert.h>
#include <winpr/synch.h>
#include <winpr/stream.h>

#include <freerdp/client/channels.h>
#include <freerdp/channels/log.h>
#include <freerdp/client/wwdisplay.h>

#define TAG CHANNELS_TAG("wwdisplay.client")

typedef struct
{
	GENERIC_DYNVC_PLUGIN base;
	WwDisplayClientContext* context;

	/* The client sets the rate on its own thread, the channel opens and
	 * closes on the channel thread. */
	CRITICAL_SECTION lock;
	IWTSVirtualChannel* channel; /* while the server has it open */
	UINT32 refresh_mhz;          /* 0 until the client knows it */
} WWDISPLAY_PLUGIN;

/* Under the lock. */
static UINT wwdisplay_send_refresh_rate(WWDISPLAY_PLUGIN* ww)
{
	BYTE pdu[8];
	wStream sbuffer = WINPR_C_ARRAY_INIT;
	wStream* s = Stream_StaticInit(&sbuffer, pdu, sizeof(pdu));

	if (!ww->channel || ww->refresh_mhz == 0)
		return CHANNEL_RC_OK;

	Stream_Write_UINT16(s, WWDISPLAY_PDU_REFRESH_RATE);
	Stream_Write_UINT16(s, 0);
	Stream_Write_UINT32(s, ww->refresh_mhz);

	WLog_Print(ww->base.log, WLOG_DEBUG, "refresh rate %" PRIu32 " mHz", ww->refresh_mhz);
	return ww->channel->Write(ww->channel, sizeof(pdu), pdu, nullptr);
}

static UINT
wwdisplay_on_data_received(WINPR_ATTR_UNUSED IWTSVirtualChannelCallback* pChannelCallback,
                           WINPR_ATTR_UNUSED wStream* data)
{
	/* The server sends nothing yet. */
	return CHANNEL_RC_OK;
}

static UINT wwdisplay_on_open(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	WINPR_ASSERT(callback);
	WWDISPLAY_PLUGIN* ww = (WWDISPLAY_PLUGIN*)callback->plugin;
	WINPR_ASSERT(ww);

	EnterCriticalSection(&ww->lock);
	ww->channel = callback->channel;
	const UINT rc = wwdisplay_send_refresh_rate(ww);
	LeaveCriticalSection(&ww->lock);
	return rc;
}

static UINT wwdisplay_on_close(IWTSVirtualChannelCallback* pChannelCallback)
{
	GENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;
	WINPR_ASSERT(callback);
	WWDISPLAY_PLUGIN* ww = (WWDISPLAY_PLUGIN*)callback->plugin;
	WINPR_ASSERT(ww);

	EnterCriticalSection(&ww->lock);
	if (ww->channel == callback->channel)
		ww->channel = nullptr;
	LeaveCriticalSection(&ww->lock);

	if (ww->base.listener_callback && ww->base.listener_callback->channel_callback == callback)
		ww->base.listener_callback->channel_callback = nullptr;
	free(callback);
	return CHANNEL_RC_OK;
}

static UINT wwdisplay_set_refresh_rate(WwDisplayClientContext* context, UINT32 refresh_mhz)
{
	WINPR_ASSERT(context);
	WWDISPLAY_PLUGIN* ww = (WWDISPLAY_PLUGIN*)context->handle;
	WINPR_ASSERT(ww);

	UINT rc = CHANNEL_RC_OK;
	EnterCriticalSection(&ww->lock);
	if (refresh_mhz != ww->refresh_mhz)
	{
		ww->refresh_mhz = refresh_mhz;
		rc = wwdisplay_send_refresh_rate(ww);
	}
	LeaveCriticalSection(&ww->lock);
	return rc;
}

static UINT wwdisplay_plugin_initialize(GENERIC_DYNVC_PLUGIN* base,
                                        WINPR_ATTR_UNUSED rdpContext* rcontext,
                                        WINPR_ATTR_UNUSED rdpSettings* settings)
{
	WWDISPLAY_PLUGIN* ww = (WWDISPLAY_PLUGIN*)base;
	WINPR_ASSERT(ww);

	WwDisplayClientContext* context =
	    (WwDisplayClientContext*)calloc(1, sizeof(WwDisplayClientContext));
	if (!context)
		return CHANNEL_RC_NO_MEMORY;

	if (!InitializeCriticalSectionAndSpinCount(&ww->lock, 0))
	{
		free(context);
		return ERROR_INTERNAL_ERROR;
	}

	context->handle = ww;
	context->SetRefreshRate = wwdisplay_set_refresh_rate;
	ww->base.iface.pInterface = ww->context = context;
	return CHANNEL_RC_OK;
}

static void wwdisplay_plugin_terminated(GENERIC_DYNVC_PLUGIN* base)
{
	WWDISPLAY_PLUGIN* ww = (WWDISPLAY_PLUGIN*)base;
	WINPR_ASSERT(ww);

	if (!ww->context)
		return;
	DeleteCriticalSection(&ww->lock);
	free(ww->context);
}

static const IWTSVirtualChannelCallback wwdisplay_callbacks = { wwdisplay_on_data_received,
	                                                            wwdisplay_on_open,
	                                                            wwdisplay_on_close, nullptr };

FREERDP_ENTRY_POINT(UINT VCAPITYPE wwdisplay_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))
{
	return freerdp_generic_DVCPluginEntry(pEntryPoints, TAG, WWDISPLAY_DVC_CHANNEL_NAME,
	                                      sizeof(WWDISPLAY_PLUGIN),
	                                      sizeof(GENERIC_CHANNEL_CALLBACK), &wwdisplay_callbacks,
	                                      wwdisplay_plugin_initialize, wwdisplay_plugin_terminated);
}
