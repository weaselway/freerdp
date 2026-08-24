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

	/**
	 * A buffer resolved to the client's mapping of the shared memory pool that
	 * backs it. Valid only until the buffer is destroyed or its pool closed,
	 * i.e. do not hold it past the PresentBuffer callback.
	 */
	typedef struct
	{
		const void* data; /* read-only view of the buffer's first pixel */
		size_t size;      /* bytes readable from data */
		UINT32 stride;
		UINT32 width;
		UINT32 height;
		UINT32 format; /* GFXREDIR_BUFFER_PIXEL_FORMAT_* */
	} GFXREDIR_BUFFER_MAPPING;

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

	typedef UINT (*pcGfxRedirPresentBufferAck)(GfxRedirClientContext* context,
	                                           const GFXREDIR_PRESENT_BUFFER_ACK_PDU* ack);

	typedef BOOL (*pcGfxRedirGetBufferMapping)(GfxRedirClientContext* context, UINT64 bufferId,
	                                           GFXREDIR_BUFFER_MAPPING* mapping);

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

		/* Set by the channel, called by the application. */
		WINPR_ATTR_NODISCARD pcGfxRedirPresentBufferAck PresentBufferAck;

		/**
		 * Resolve a bufferId to the mapped shared memory behind it. Returns
		 * FALSE if the buffer is unknown, or if its pool could not be mapped
		 * (no /wslgsharedmemorypath, or the geometry does not fit the pool).
		 */
		WINPR_ATTR_NODISCARD pcGfxRedirGetBufferMapping GetBufferMapping;

		/**
		 * The server allows only one present in flight per window and waits for
		 * an ack before sending the next one, so an unacked present stalls the
		 * stream. By default the channel acks each PRESENT_BUFFER itself, as
		 * soon as the PresentBuffer callback returns.
		 *
		 * An application that consumes the buffer asynchronously should set
		 * this to TRUE and call PresentBufferAck() once it is actually done
		 * reading the buffer -- acking early tells the server it may reuse the
		 * memory, which would tear. Set it from the CapsConfirm callback or
		 * earlier; the channel reads it on every present.
		 */
		BOOL deferPresentBufferAck;
	};

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_CHANNEL_GFXREDIR_CLIENT_GFXREDIR_H */
