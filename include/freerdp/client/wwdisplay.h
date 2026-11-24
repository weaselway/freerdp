/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Weaselway display information channel
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

#ifndef FREERDP_CHANNEL_WWDISPLAY_CLIENT_WWDISPLAY_H
#define FREERDP_CHANNEL_WWDISPLAY_CLIENT_WWDISPLAY_H

#include <freerdp/channels/wwdisplay.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct s_wwdisplay_client_context WwDisplayClientContext;

	/* Remembers the rate of a monitor (< WWDISPLAY_MAX_MONITORS) and sends it
	 * if the server has opened the channel; otherwise it goes out when the
	 * server does. 0 is not sent. */
	typedef UINT (*pcWwDisplaySetRefreshRate)(WwDisplayClientContext* context, UINT16 monitor,
	                                          UINT32 refresh_mhz);

	/* Sends WWDISPLAY_PDU_PRESENT_TIMING if the server has the channel open;
	 * otherwise it is dropped. */
	typedef UINT (*pcWwDisplaySendPresentTiming)(WwDisplayClientContext* context, UINT16 monitor,
	                                             UINT32 frames, INT32 slack_us);

	/* Set by the client: the server's WWDISPLAY_INHIBIT_* flags, on the channel's
	 * thread. 0 again when the channel closes. */
	typedef void (*pcWwDisplayInhibit)(WwDisplayClientContext* context, UINT32 flags);

	struct s_wwdisplay_client_context
	{
		void* handle;
		void* custom;

		pcWwDisplaySetRefreshRate SetRefreshRate;
		pcWwDisplaySendPresentTiming SendPresentTiming;
		pcWwDisplayInhibit Inhibit;
	};

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_CHANNEL_WWDISPLAY_CLIENT_WWDISPLAY_H */
