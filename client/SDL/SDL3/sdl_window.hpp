/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client
 *
 * Copyright 2023 Armin Novak <armin.novak@thincast.com>
 * Copyright 2023 Thincast Technologies GmbH
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
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <freerdp/settings_types.h>

class SdlD3D11Presenter;

class SdlWindow
{
  public:
	[[nodiscard]] static SdlWindow create(SDL_DisplayID id, const std::string& title, Uint32 flags,
	                                      Uint32 width = 0, Uint32 height = 0);
	[[nodiscard]] static rdpMonitor query(SDL_DisplayID id, bool forceAsPrimary = false);

	SdlWindow(SdlWindow&& other) noexcept;
	SdlWindow(const SdlWindow& other) = delete;
	virtual ~SdlWindow();

	SdlWindow& operator=(const SdlWindow& other) = delete;
	SdlWindow& operator=(SdlWindow&& other) = delete;

	[[nodiscard]] SDL_WindowID id() const;
	[[nodiscard]] SDL_DisplayID displayIndex() const;
	[[nodiscard]] SDL_Rect rect() const;
	[[nodiscard]] SDL_Rect bounds() const;
	[[nodiscard]] SDL_Window* window() const;
	[[nodiscard]] SDL_Renderer* renderer() const;

	/* /sdl-presenter (Windows, on by default): draw with our own Direct3D 11
	 * device instead of an SDL_Renderer. Process wide, set before any window
	 * exists; turned off again if a window can't set it up. /sdl-show-damage
	 * needs the SDL renderer and wins. A window drawing this way must only be
	 * drawn to from the context's render thread. */
	static void setPresenter(bool enable);
	[[nodiscard]] static bool presenterEnabled();
	[[nodiscard]] bool hasPresenter() const;

	/* For the render thread. presentHandle() is a Win32 HANDLE to wait on
	 * before updateSurface() once presentPending() is set; presentSlot() tells
	 * the window that wait succeeded. */
	[[nodiscard]] void* presentHandle() const;
	[[nodiscard]] bool presentPending() const;
	[[nodiscard]] bool hasPresentSlot() const;
	void presentSlot();
	/* True once after the presenter lost what it had drawn (device lost) and
	 * needs the whole desktop again. */
	[[nodiscard]] bool takeRedrawRequest();

	[[nodiscard]] Sint32 offsetX() const;
	void setOffsetX(Sint32 x);

	void setOffsetY(Sint32 y);
	[[nodiscard]] Sint32 offsetY() const;

	[[nodiscard]] rdpMonitor monitor(bool isPrimary) const;
	void setMonitor(rdpMonitor monitor);

	[[nodiscard]] float scale() const;
	[[nodiscard]] SDL_DisplayOrientation orientation() const;

	[[nodiscard]] bool grabKeyboard(bool enable);
	[[nodiscard]] bool grabMouse(bool enable);
	void setBordered(bool bordered);
	void raise();
	void resizeable(bool use);
	void fullscreen(bool enter, bool forceOriginalDisplay);
	void minimize();

	[[nodiscard]] bool resizeToScale();
	[[nodiscard]] bool resize(const SDL_Point& size);

	[[nodiscard]] bool drawRect(SDL_Surface* surface, SDL_Point offset, const SDL_Rect& srcRect);
	[[nodiscard]] bool drawRects(SDL_Surface* surface, SDL_Point offset,
	                             const std::vector<SDL_Rect>& rects = {});
	[[nodiscard]] bool drawScaledRect(SDL_Surface* surface, const SDL_FPoint& scale,
	                                  const SDL_Rect& srcRect);

	[[nodiscard]] bool drawScaledRects(SDL_Surface* surface, const SDL_FPoint& scale,
	                                   const std::vector<SDL_Rect>& rects = {});

	[[nodiscard]] bool fill(Uint8 r = 0x00, Uint8 g = 0x00, Uint8 b = 0x00, Uint8 a = 0xff);
	[[nodiscard]] bool blit(SDL_Surface* surface, const SDL_Rect& src, SDL_Rect& dst);
	void updateSurface();

	/* /sdl-show-damage: tint the regions updated this frame, to make the
	 * incremental update path visible. Process wide, set from the command
	 * line before any window exists. */
	static void setShowDamage(bool enable);

	/* /sdl-show-stats[:<scale>]: draw a counter overlay in the top left corner.
	 * The scale, if > 0, is the text scale; the built-in debug font is 8px,
	 * which is unreadable on a HiDPI panel. Process wide. */
	static void setShowStats(bool enable, float scale = 2.0f);
	[[nodiscard]] static bool showStats();

	/* /sdl-no-vsync: present without waiting for the refresh. Tears, but takes
	 * the vsync wait out of the frame path, to tell it apart from real work. */
	static void setVSync(bool enable);

	/* /sdl-stats-log:<file>: append the overlay's lines to a file. The console
	 * is released at startup unless it was inherited from a Windows shell, so
	 * stderr goes nowhere when started from WSL. Turns the stats on. */
	static bool setStatsLog(const char* path);


	/* Where a frame's time goes, in the order the stages run. Each is summed
	 * over one frame (a frame can be several blits) and shown by the stats
	 * overlay as avg/max per sampling window. */
	enum StatStage
	{
		STAT_WAIT,      /* gfxredir present queued -> SDL thread starts on it */
		STAT_LOCK,      /* waiting for the context lock */
		STAT_UPLOAD,    /* SDL_UpdateTexture: shared memory -> GPU texture */
		STAT_RENDER,    /* texture -> render target, flushed */
		STAT_ACK,       /* returning superseded buffers to the server */
		STAT_SWAPWAIT,  /* presenter only: uploaded, waiting for the swap chain */
		STAT_COMPOSITE, /* render target -> backbuffer and overlays, flushed */
		STAT_PRESENT,   /* SDL_RenderPresent */
		STAT_TOTAL,     /* gfxredir present queued -> SDL_RenderPresent returned */
		STAT_COUNT
	};

	/* SDL thread only. No-ops unless showStats() is on. */
	static void addStat(StatStage stage, Uint64 ns);
	/* SDL_GetTicksNS() at which the frame about to be drawn was queued. */
	static void setFrameQueuedAt(Uint64 ns);
	/* Any thread: a frame arrived from the server. */
	static void countFrameIn();

  protected:
	SdlWindow(SDL_DisplayID id, const std::string& title, const SDL_Rect& rect, Uint32 flags);

	[[nodiscard]] static bool fill(SDL_Window* window, Uint8 r = 0x00, Uint8 g = 0x00,
	                               Uint8 b = 0x00, Uint8 a = 0xff);
	[[nodiscard]] static rdpMonitor query(SDL_Window* window, SDL_DisplayID id,
	                                      bool forceAsPrimary = false);
	[[nodiscard]] static SDL_Rect rect(SDL_Window* window, bool forceAsPrimary = false);
	[[nodiscard]] static SDL_Rect rect(SDL_DisplayID id, bool forceAsPrimary = false);

	[[nodiscard]] static bool tryFallback(bool isFullscreen);

	enum HighDPIMode
	{
		MODE_INVALID,
		MODE_NONE,
		MODE_WINDOWS,
		MODE_MACOS
	};

	[[nodiscard]] static enum HighDPIMode isHighDPIWindowsMode(SDL_Window* window);

  private:
	void ensureRenderTarget();

	[[nodiscard]] static bool showDamage();
	[[nodiscard]] static bool vsync();
	static void commitFrameStats();
	[[nodiscard]] static float statsScale();

	/* Draws the overlay onto the backbuffer. Call with the render target unset
	 * and immediately before presenting. */
	void renderStats();
	/* The part of renderStats() that computes and logs the numbers. True if
	 * they changed. */
	bool updateStats();
	void renderStatsText(SDL_Renderer* renderer);
	[[nodiscard]] SDL_FRect statsBox() const;
	void updatePresenterOverlay();

	[[nodiscard]] bool presenterUpload(SDL_Surface* surface, SDL_Point offset,
	                                   const SDL_FPoint& scale, const std::vector<SDL_Rect>& rects);
	void presenterPresent();

	/* Regions blitted since the last present, in render-target coordinates.
	 * Only collected when showDamage() is on. */
	std::vector<SDL_Rect> _damageRects;

	/* Counters for the overlay, accumulated over a sampling window and only
	 * touched when showStats() is on. */
	Uint64 _statsWindowStart = 0; /* SDL_GetTicksNS() at the window's start */
	Uint64 _statsFrames = 0;      /* presents */
	Uint64 _statsBlits = 0;       /* SDL_UpdateTexture calls */
	Uint64 _statsBytes = 0;       /* bytes handed to SDL_UpdateTexture */
	std::vector<std::string> _statsText; /* last formatted result, redrawn every frame */

	std::unique_ptr<SdlD3D11Presenter> _presenter;
	bool _presentPending = false; /* uploaded, not presented yet */
	bool _presentSlot = false;    /* the swap chain will take a frame */
	Uint64 _presentPendingSince = 0;

	SDL_Window* _window = nullptr;
	SDL_Renderer* _renderer = nullptr;
	SDL_Texture* _renderTarget = nullptr;
	SDL_Texture* _gdiTexture = nullptr;
	int _gdiTextureW = 0;
	int _gdiTextureH = 0;
	int _initialW = 0;
	int _initialH = 0;
	SDL_DisplayID _displayID = 0;
	Sint32 _offset_x = 0;
	Sint32 _offset_y = 0;
	rdpMonitor _monitor{};
};
