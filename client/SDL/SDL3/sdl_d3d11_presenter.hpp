/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client, Direct3D 11 presenter
 *
 * Copyright 2026 The FreeRDP Contributors
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

#pragma once

#include <memory>
#include <vector>

#include <SDL3/SDL.h>

/* Draws the remote desktop into a window with its own Direct3D 11 device
 * instead of an SDL_Renderer (/sdl-presenter). Two things SDL's renderer
 * cannot do are the point:
 *
 *  - The swap chain is waitable, so nothing blocks inside Present(). The
 *    caller waits on waitHandle() instead, and can keep uploading newer frames
 *    while it does: the upload of frame N+1 overlaps the vsync wait of frame N.
 *  - Uploads go through a small ring of persistent staging textures. SDL's
 *    D3D11 renderer allocates and frees one per SDL_UpdateTexture call.
 *
 * It holds the whole desktop in one texture, so a present needs nothing from
 * the source surface. A lost device (driver update, GPU reset) is set up again
 * on the next upload. None of this is thread safe: apart from create() and the
 * destructor it is meant to be driven by one render thread.
 */
class SdlD3D11Presenter
{
  public:
	/* hwnd is the window's HWND. nullptr if Direct3D 11 can't be set up. */
	[[nodiscard]] static std::unique_ptr<SdlD3D11Presenter> create(void* hwnd, bool vsync);

	SdlD3D11Presenter(const SdlD3D11Presenter& other) = delete;
	SdlD3D11Presenter(SdlD3D11Presenter&& other) = delete;
	SdlD3D11Presenter& operator=(const SdlD3D11Presenter& other) = delete;
	SdlD3D11Presenter& operator=(SdlD3D11Presenter&& other) = delete;
	~SdlD3D11Presenter();

	/* Where the desktop goes in the window: top left corner and scale, in
	 * window pixels. */
	void setMapping(SDL_Point offset, SDL_FPoint scale);

	/* Copy rects (surface coordinates; empty = everything) out of the surface.
	 * bytes, if given, receives the amount copied. */
	[[nodiscard]] bool upload(SDL_Surface* surface, const std::vector<SDL_Rect>& rects,
	                          Uint64* bytes = nullptr);

	/* A Win32 HANDLE, signalled when the swap chain takes another frame. Every
	 * present() has to be preceded by one successful wait on it. */
	[[nodiscard]] void* waitHandle() const;

	/* Draw the desktop to the back buffer; then hand it to the compositor.
	 * Neither waits for vsync. */
	[[nodiscard]] bool draw();
	[[nodiscard]] bool present();

	/* A BGRA32 surface with premultiplied alpha drawn over the top left
	 * corner from the next draw() on; nullptr removes it. */
	[[nodiscard]] bool setOverlay(SDL_Surface* surface);

	/* True once after the device was lost and set up again: the desktop
	 * texture is empty and needs a full upload. */
	[[nodiscard]] bool takeRedrawRequest();

  private:
	struct Impl;
	explicit SdlD3D11Presenter(std::unique_ptr<Impl> impl);

	std::unique_ptr<Impl> _impl;
};
