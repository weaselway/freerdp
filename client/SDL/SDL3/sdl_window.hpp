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

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <freerdp/settings_types.h>
#include <freerdp/client/wwdisplay.h>

class SdlD3D11Presenter;
class SdlWin32VBlank;

/* Characters [begin, end) of a text line drawn in a colour (0xRRGGBB). */
struct SdlTextColor
{
	size_t line;
	size_t begin;
	size_t end;
	Uint32 rgb;
};

class SdlWindow
{
  public:
	/* presenter: draw with the Direct3D 11 presenter if it is enabled, see
	 * setPresenter(). Only for desktop windows; RAIL windows need a renderer. */
	[[nodiscard]] static SdlWindow create(SDL_DisplayID id, const std::string& title, Uint32 flags,
	                                      Uint32 width = 0, Uint32 height = 0,
	                                      bool presenter = false);
	/* Create at an explicit position+size (RAIL windows: server-driven geometry). */
	[[nodiscard]] static SdlWindow create(SDL_DisplayID id, const std::string& title, Uint32 flags,
	                                      const SDL_Rect& rect);
	/* Popup: no taskbar entry, no keyboard focus, Wayland-positionable (xdg_popup). */
	[[nodiscard]] static SdlWindow createPopup(SDL_Window* parent, const SDL_Rect& rect,
	                                           bool transparent, bool tooltip = false);
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
	 * exists; turned off again if a window can't set it up. A window drawing
	 * this way must only be drawn to from the context's render thread. */
	static void setPresenter(bool enable);
	[[nodiscard]] static bool presenterEnabled();

	/* /sdl-zero-copy (Windows, on by default): let the presenter copy
	 * gfxredir frames on the GPU straight out of the shared memory, through
	 * Direct3D 12. Off, or where that is not possible, the CPU copies them
	 * into a staging texture. Process wide, set before any window exists. */
	static void setZeroCopy(bool enable);
	[[nodiscard]] static bool zeroCopyEnabled();
	[[nodiscard]] bool hasPresenter() const;

	/* For the render thread. presentHandle() is a Win32 HANDLE to wait on
	 * before updateSurface() once presentPending() is set; presentSlot() tells
	 * the window that wait succeeded. */
	[[nodiscard]] void* presentHandle() const;
	/* True while a zero copy still reads the gfxredir buffer it was given:
	 * no acks until then. copyHandle() is a Win32 HANDLE set when it is done. */
	[[nodiscard]] bool copyPending() const;
	[[nodiscard]] void* copyHandle() const;
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

	/* The window's index in the monitor layout sent to the server, which is
	 * also the gfxredir windowId the server draws it with. */
	[[nodiscard]] UINT16 monitorIndex() const;
	void setMonitorIndex(UINT16 index);

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
	/* Render interactive resize feedback overlay. */
	bool paintResizeFrame(SDL_Surface* surface, SDL_Point off, bool contentChanged,
	                      const SDL_Rect& inset = { 0, 0, 0, 0 }, bool fillRevealed = true,
	                      bool dashedBorder = true);
	void updateSurface();

	/* /sdl-show-damage: outline the regions updated this frame, to make the
	 * incremental update path visible. Process wide; the damage hotkey
	 * toggles it from the SDL thread. */
	static void setShowDamage(bool enable);
	[[nodiscard]] static bool showDamage();

	/* /sdl-show-stats[:<scale>]: draw a counter overlay in the top left corner.
	 * The scale, if > 0, is the text scale; the built-in debug font is 8px,
	 * which is unreadable on a HiDPI panel. Process wide; the stats hotkey
	 * toggles it from the SDL thread. */
	static void setShowStats(bool enable, float scale = 2.0f);
	[[nodiscard]] static bool showStats();

	/* /sdl-no-vsync: present without waiting for the refresh. Tears, but takes
	 * the vsync wait out of the frame path, to tell it apart from real work. */
	static void setVSync(bool enable);

	/* Where presented frames land relative to the monitor's vblank goes to
	 * weaselwayd through this channel, while it is set. Process wide. */
	static void setPresentTimingChannel(WwDisplayClientContext* ww);

	/* /sdl-stats-log:<file>: append the overlay's lines to a file. The console
	 * is released at startup unless it was inherited from a Windows shell, so
	 * stderr goes nowhere when started from WSL. Turns the stats on. */
	static bool setStatsLog(const char* path);

	/* A notification centered at the top, fontSize in points; empty text
	 * hides it. SDL thread. Shows from the next present, so ask for a redraw
	 * when this returns true. Only the presenter draws it. */
	[[nodiscard]] bool setNotification(const std::string& text, float fontSize = 15.0f);

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

	/* The overlay's numbers for one window, so windows at different refresh
	 * rates each show their own frames. frame collects the frame being drawn
	 * and is folded into stages when it is presented. */
	struct FrameStats
	{
		struct Stage
		{
			Uint64 sum = 0;
			Uint64 max = 0;
			Uint64 count = 0;
		};
		/* One frame for the graphs. */
		struct Sample
		{
			std::array<float, STAT_COUNT> ms{}; /* stage times */
			float intervalMs = 0.0f; /* since the window's previous present, 0 for the first */
			bool late = false;
			bool dropped = false; /* it replaced frames that were never shown */
		};
		std::array<Stage, STAT_COUNT> stages{};
		std::array<Uint64, STAT_COUNT> frame{};
		std::array<bool, STAT_COUNT> frameHas{};
		Uint64 frameQueuedAt = 0;
		Uint64 frameDropped = 0; /* replaced by the frame being drawn, never shown */
		/* Per sampling window: frames that arrived and the time between them,
		 * each frame's latency (queued -> presented), and of the frames whose
		 * deadline is known, how many missed it. */
		Uint64 framesIn = 0;
		std::vector<float> arrivalMs;
		std::vector<float> latencyMs;
		Uint64 lateFrames = 0;
		Uint64 deadlineFrames = 0;
		Uint64 droppedFrames = 0;
		/* The last frames for the graphs, oldest at historyNext once full. */
		std::array<Sample, 512> history{};
		size_t historyNext = 0;
		size_t historyCount = 0;
	};

	/* The drawing thread only, like everything below. No-ops unless
	 * showStats() is on. */
	void addStat(StatStage stage, Uint64 ns);
	/* STAT_ACK: an ack delays the frame drawn and not shown yet, if any. */
	void addAckStat(Uint64 ns);
	/* SDL_GetTicksNS() at which the frame about to be drawn was queued, and
	 * how many frames that arrived since the last one drawn it replaces. */
	void setFrameQueuedAt(Uint64 ns, Uint64 skipped);
	/* Frames arrived from the server, each intervalNs after the one before,
	 * 0 for the first. */
	void countFramesIn(const std::vector<Uint64>& intervalsNs);

  protected:
	SdlWindow(SDL_DisplayID id, const std::string& title, const SDL_Rect& rect, Uint32 flags,
	          bool presenter = false);
	SdlWindow(SDL_Window* parent, const SDL_Rect& rect, bool transparent, bool tooltip);

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
	bool ensureGdiTexture(SDL_Surface* surface);

	[[nodiscard]] static bool vsync();
	/* Folds the frame's stage times into the stats. intervalNs is the time
	 * since the window's previous present; late is 1 if the frame missed its
	 * vblank, 0 if not, -1 if unknown. */
	void commitFrameStats(Uint64 intervalNs, int late);
	/* A frame was drawn and not presented yet. */
	bool _frameUnshown = false;
	FrameStats _frameStats;
	/* See commitFrameStats(). */
	[[nodiscard]] int missedVBlank(Uint64 queuedNs, Uint64 nowNs);
	[[nodiscard]] Uint64 frameInterval(Uint64 nowNs);
	/* The monitor's vblank, created on first use; nullptr if unknown. */
	[[nodiscard]] SdlWin32VBlank* vblank();
	Uint64 _lastPresentNs = 0;
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
	/* Follows setShowStats() from the drawing thread. */
	void syncStatsToggle();
	bool _statsOn = false;

	[[nodiscard]] bool presenterUpload(SDL_Surface* surface, SDL_Point offset,
	                                   const SDL_FPoint& scale, const std::vector<SDL_Rect>& rects);
	void presenterPresent(const std::vector<SDL_Rect>& damage);
	void recordPresentTiming();

	/* Regions blitted since the last present, in render-target coordinates;
	 * with the presenter in window pixels. Only collected when showDamage()
	 * is on. */
	std::vector<SDL_Rect> _damageRects;

	/* Counters for the overlay, accumulated over a sampling window and only
	 * touched when showStats() is on. */
	Uint64 _statsWindowStart = 0; /* SDL_GetTicksNS() at the window's start */
	Uint64 _statsFrames = 0;      /* presents */
	Uint64 _statsBlits = 0;       /* SDL_UpdateTexture calls */
	Uint64 _statsBytes = 0;       /* bytes handed to SDL_UpdateTexture */
	/* Presenter uploads by how they were copied: [0] by the CPU into staging,
	 * [1] zero copy. busy is the time spent in upload(). */
	struct UploadStat
	{
		Uint64 count = 0;
		Uint64 bytes = 0;
		Uint64 busyNS = 0;
	};
	std::array<UploadStat, 2> _statsUploads{};
	std::vector<std::string> _statsText; /* last formatted result, redrawn every frame */
	std::vector<SdlTextColor> _statsColors; /* parts of _statsText in a colour */

	std::unique_ptr<SdlD3D11Presenter> _presenter;
	bool _presentPending = false; /* uploaded, not presented yet */
	bool _presentSlot = false;    /* the swap chain will take a frame */
	Uint64 _presentPendingSince = 0;
	Sint64 _presentPendingQpc = 0; /* QueryPerformanceCounter() at the same time */
	/* For recordPresentTiming(): the monitor's vblank, once needed, and the
	 * slack per frame since _timingSince. */
	std::unique_ptr<SdlWin32VBlank> _vblank;
	std::vector<Sint32> _timingSlackUs;
	Uint64 _timingSince = 0;

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
	std::atomic<UINT16> _monitorIndex = 0; /* read by the render thread */

	std::string _notificationText; /* SDL thread */
	/* Handed to the presenter on the render thread. Kept to set it again after
	 * the presenter lost its device. */
	std::mutex _notificationLock;
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> _notification{
		nullptr, SDL_DestroySurface
	};
	bool _notificationChanged = false;
};
