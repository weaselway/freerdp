/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDPXXXX Remote App Graphics Redirection Virtual Channel Extension
 *
 * Client channel interface.
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

#ifndef FREERDP_CHANNEL_GFXREDIR_CLIENT_GFXREDIR_H
#define FREERDP_CHANNEL_GFXREDIR_CLIENT_GFXREDIR_H

#include <freerdp/channels/gfxredir.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct s_gfxredir_client_context GfxRedirClientContext;

	/* Every callback is optional; leave one nullptr to ignore that PDU. The
	 * channel handles the caps handshake itself and only reports the outcome.
	 * All of these are invoked on the drdynvc receive thread. */

	typedef UINT (*pcGfxRedirCapsConfirm)(GfxRedirClientContext* context, UINT32 version);
	typedef UINT (*pcGfxRedirError)(GfxRedirClientContext* context,
	                                const GFXREDIR_ERROR_PDU* error);
	typedef UINT (*pcGfxRedirOpenPool)(GfxRedirClientContext* context,
	                                   const GFXREDIR_OPEN_POOL_PDU* openPool);
	typedef UINT (*pcGfxRedirClosePool)(GfxRedirClientContext* context,
	                                    const GFXREDIR_CLOSE_POOL_PDU* closePool);
	typedef UINT (*pcGfxRedirCreateBuffer)(GfxRedirClientContext* context,
	                                       const GFXREDIR_CREATE_BUFFER_PDU* createBuffer);
	typedef UINT (*pcGfxRedirDestroyBuffer)(GfxRedirClientContext* context,
	                                        const GFXREDIR_DESTROY_BUFFER_PDU* destroyBuffer);
	typedef UINT (*pcGfxRedirPresentBuffer)(GfxRedirClientContext* context,
	                                        const GFXREDIR_PRESENT_BUFFER_PDU* presentBuffer);

	struct s_gfxredir_client_context
	{
		void* handle;
		void* custom;

		/* Server -> client notifications. */
		pcGfxRedirCapsConfirm CapsConfirm;
		pcGfxRedirError Error;
		pcGfxRedirOpenPool OpenPool;
		pcGfxRedirClosePool ClosePool;
		pcGfxRedirCreateBuffer CreateBuffer;
		pcGfxRedirDestroyBuffer DestroyBuffer;
		pcGfxRedirPresentBuffer PresentBuffer;
	};

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_CHANNEL_GFXREDIR_CLIENT_GFXREDIR_H */
