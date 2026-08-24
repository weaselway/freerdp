/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client Channels
 *
 * Copyright 2022 Armin Novak <armin.novak@thincast.com>
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

#include <freerdp/client/rail.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/gfxredir.h>
#include <freerdp/channels/rdpewa.h>

#include "sdl_channels.hpp"
#include "sdl_context.hpp"
#include "sdl_disp.hpp"

/* gfxredir: shared-memory graphics redirection. The server writes frames into a
 * shared memory pool and tells us which rectangle changed; the SDL thread then
 * uploads that rectangle to its texture straight out of the mapping, with no
 * intermediate copy. Callbacks run on the drdynvc receive thread. */

static UINT sdl_gfxredir_caps_confirm(GfxRedirClientContext* context, UINT32 version)
{
	auto sdl = static_cast<SdlContext*>(context->custom);
	if (!sdl) /* channel already disconnected */
		return CHANNEL_RC_OK;
	WLog_Print(sdl->getWLog(), WLOG_INFO, "gfxredir: negotiated caps version 0x%" PRIx32, version);
	return CHANNEL_RC_OK;
}

static UINT sdl_gfxredir_open_pool(GfxRedirClientContext* context,
                                   const GFXREDIR_OPEN_POOL_PDU* openPool)
{
	auto sdl = static_cast<SdlContext*>(context->custom);
	if (!sdl) /* channel already disconnected */
		return CHANNEL_RC_OK;
	WLog_Print(sdl->getWLog(), WLOG_INFO,
	           "gfxredir: OpenPool poolId=%" PRIu64 " poolSize=%" PRIu64, openPool->poolId,
	           openPool->poolSize);
	return CHANNEL_RC_OK;
}

static UINT sdl_gfxredir_close_pool(GfxRedirClientContext* context,
                                    const GFXREDIR_CLOSE_POOL_PDU* closePool)
{
	auto sdl = static_cast<SdlContext*>(context->custom);
	if (!sdl) /* channel already disconnected */
		return CHANNEL_RC_OK;
	WLog_Print(sdl->getWLog(), WLOG_INFO, "gfxredir: ClosePool poolId=%" PRIu64,
	           closePool->poolId);
	/* Our surface wraps this pool's memory; it must not outlive the mapping. */
	sdl->gfxRedirReset();
	return CHANNEL_RC_OK;
}

static UINT sdl_gfxredir_create_buffer(GfxRedirClientContext* context,
                                       const GFXREDIR_CREATE_BUFFER_PDU* createBuffer)
{
	auto sdl = static_cast<SdlContext*>(context->custom);
	if (!sdl) /* channel already disconnected */
		return CHANNEL_RC_OK;
	WLog_Print(sdl->getWLog(), WLOG_INFO,
	           "gfxredir: CreateBuffer bufferId=%" PRIu64 " %" PRIu32 "x%" PRIu32
	           " stride=%" PRIu32 " format=%" PRIu32,
	           createBuffer->bufferId, createBuffer->width, createBuffer->height,
	           createBuffer->stride, createBuffer->format);
	return CHANNEL_RC_OK;
}

static UINT sdl_gfxredir_destroy_buffer(GfxRedirClientContext* context,
                                        const GFXREDIR_DESTROY_BUFFER_PDU* destroyBuffer)
{
	auto sdl = static_cast<SdlContext*>(context->custom);
	if (!sdl) /* channel already disconnected */
		return CHANNEL_RC_OK;
	WLog_Print(sdl->getWLog(), WLOG_INFO, "gfxredir: DestroyBuffer bufferId=%" PRIu64,
	           destroyBuffer->bufferId);
	sdl->gfxRedirReset();
	return CHANNEL_RC_OK;
}

static UINT sdl_gfxredir_present_buffer(GfxRedirClientContext* context,
                                        const GFXREDIR_PRESENT_BUFFER_PDU* presentBuffer)
{
	auto sdl = static_cast<SdlContext*>(context->custom);
	if (!sdl) /* channel already disconnected */
		return CHANNEL_RC_OK;
	WLog_Print(sdl->getWLog(), WLOG_DEBUG,
	           "gfxredir: PresentBuffer presentId=%" PRIu64 " bufferId=%" PRIu64
	           " rect=%" PRIu32 "x%" PRIu32 "+%" PRIu32 "+%" PRIu32 " target=%" PRIu32
	           "x%" PRIu32,
	           presentBuffer->presentId, presentBuffer->bufferId, presentBuffer->dirtyRect.width,
	           presentBuffer->dirtyRect.height, presentBuffer->dirtyRect.left,
	           presentBuffer->dirtyRect.top, presentBuffer->targetWidth,
	           presentBuffer->targetHeight);

	/* Publishes the shared memory to the SDL thread, which uploads it to the
	 * texture and then acks. Nothing is copied here. */
	if (!sdl->gfxRedirQueuePresent(context, presentBuffer))
	{
		/* Nothing will be drawn, so nothing will ack either -- release the
		 * frame now or the server stops sending. */
		GFXREDIR_PRESENT_BUFFER_ACK_PDU ack = {};
		ack.windowId = presentBuffer->windowId;
		ack.presentId = presentBuffer->presentId;
		return context->PresentBufferAck(context, &ack);
	}

	return CHANNEL_RC_OK;
}

void sdl_OnChannelConnectedEventHandler(void* context, const ChannelConnectedEventArgs* e)
{
	auto sdl = get_context(context);

	WINPR_ASSERT(sdl);
	WINPR_ASSERT(e);

	if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		auto clip = reinterpret_cast<CliprdrClientContext*>(e->pInterface);
		WINPR_ASSERT(clip);

		if (!sdl->getClipboardChannelContext().init(clip))
			WLog_Print(sdl->getWLog(), WLOG_WARN, "Failed to initialize clipboard channel");
	}
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		auto disp = reinterpret_cast<DispClientContext*>(e->pInterface);
		WINPR_ASSERT(disp);

		if (!sdl->getDisplayChannelContext().init(disp))
			WLog_Print(sdl->getWLog(), WLOG_WARN, "Failed to initialize display channel");
	}
	else if (strcmp(e->name, GFXREDIR_DVC_CHANNEL_NAME) == 0)
	{
		auto redir = reinterpret_cast<GfxRedirClientContext*>(e->pInterface);
		WINPR_ASSERT(redir);

		redir->custom = sdl;
		/* The texture upload happens later, on the SDL thread, so the channel
		 * must not ack on our behalf when the callback returns. */
		redir->deferPresentBufferAck = TRUE;
		redir->CapsConfirm = sdl_gfxredir_caps_confirm;
		redir->OpenPool = sdl_gfxredir_open_pool;
		redir->ClosePool = sdl_gfxredir_close_pool;
		redir->CreateBuffer = sdl_gfxredir_create_buffer;
		redir->DestroyBuffer = sdl_gfxredir_destroy_buffer;
		redir->PresentBuffer = sdl_gfxredir_present_buffer;

		WLog_Print(sdl->getWLog(), WLOG_INFO, "gfxredir channel connected");
	}
	else
		freerdp_client_OnChannelConnectedEventHandler(context, e);
}

void sdl_OnChannelDisconnectedEventHandler(void* context, const ChannelDisconnectedEventArgs* e)
{
	auto sdl = get_context(context);

	WINPR_ASSERT(sdl);
	WINPR_ASSERT(e);

	// TODO: Set resizeable depending on disp channel and /dynamic-resolution
	if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		auto clip = reinterpret_cast<CliprdrClientContext*>(e->pInterface);
		WINPR_ASSERT(clip);

		if (!sdl->getClipboardChannelContext().uninit(clip))
			WLog_Print(sdl->getWLog(), WLOG_WARN, "Failed to uninitialize clipboard channel");
		clip->custom = nullptr;
	}
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		auto disp = reinterpret_cast<DispClientContext*>(e->pInterface);
		WINPR_ASSERT(disp);

		if (!sdl->getDisplayChannelContext().uninit(disp))
			WLog_Print(sdl->getWLog(), WLOG_WARN, "Failed to uninitialize display channel");
		disp->custom = nullptr;
	}
	else if (strcmp(e->name, GFXREDIR_DVC_CHANNEL_NAME) == 0)
	{
		auto redir = reinterpret_cast<GfxRedirClientContext*>(e->pInterface);
		WINPR_ASSERT(redir);

		WLog_Print(sdl->getWLog(), WLOG_INFO, "gfxredir channel disconnected");
		sdl->gfxRedirReset();
		redir->custom = nullptr;
	}
	else
		freerdp_client_OnChannelDisconnectedEventHandler(context, e);
}

void sdl_OnUserNotificationEventHandler(void* context, const UserNotificationEventArgs* e)
{
	WINPR_UNUSED(context);
	WINPR_ASSERT(e);
	WINPR_ASSERT(e->e.Sender);

	if (strcmp(e->e.Sender, RDPEWA_CHANNEL_NAME) != 0)
		return;

	if (e->cancelPreviousNotification)
		return;

	struct userdata
	{
		std::string sender;
		std::string message;
		uint32_t timeoutMS = 0;
	};

	auto ud = new struct userdata;
	if (e->message)
		ud->message = e->message;
	if (e->e.Sender)
		ud->sender = e->e.Sender;
	ud->timeoutMS = e->timeoutMS;

	SDL_RunOnMainThread(
	    [](void* userdata)
	    {
		    auto ed = static_cast<struct userdata*>(userdata);
		    assert(ed);
		    auto parent = SDL_GetMouseFocus();
		    if (!parent)
			    parent = SDL_GetKeyboardFocus();
		    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, ed->sender.c_str(),
		                             ed->message.c_str(), parent);
		    delete ed;
	    },
	    ud, false);
}
