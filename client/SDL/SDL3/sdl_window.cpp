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
#include <SDL3/SDL_render.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <limits>
#include <sstream>
#include <cmath>
#include <utility>

#include "sdl_window.hpp"
#include "sdl_utils.hpp"

#if defined(_WIN32)
#include "sdl_d3d11_presenter.hpp"
#else
/* Never created, but the window holds a pointer to one. */
class SdlD3D11Presenter
{
};
#endif

#include <freerdp/utils/string.h>

SdlWindow::SdlWindow(SDL_DisplayID id, const std::string& title, const SDL_Rect& rect,
                     [[maybe_unused]] Uint32 flags, [[maybe_unused]] bool presenter)
    : _initialW(rect.w), _initialH(rect.h), _displayID(id)
{
	float pd = SDL_GetDisplayContentScale(id);
	if (pd <= 0.0f)
		pd = 1.0f;
	const int createW = static_cast<int>(std::ceil(static_cast<float>(rect.w) / pd));
	const int createH = static_cast<int>(std::ceil(static_cast<float>(rect.h) / pd));

	auto props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, rect.x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, rect.y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, createW);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, createH);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);

	if (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);

	if (flags & SDL_WINDOW_FULLSCREEN)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);

	if (flags & SDL_WINDOW_BORDERLESS)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);

	if (flags & SDL_WINDOW_TRANSPARENT)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, true);

	/* RAIL windows are created hidden until first paint to avoid black flash. */
	if (flags & SDL_WINDOW_HIDDEN)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);

	_window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);
	SDL_SetHint(SDL_HINT_APP_NAME, "");
	std::ignore = SDL_SyncWindow(_window);

#if defined(_WIN32)
	if (presenter && presenterEnabled() && !showDamage())
	{
		auto hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(_window),
		                                   SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
		_presenter = SdlD3D11Presenter::create(hwnd, vsync());
		if (!_presenter)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
			            "Direct3D 11 presenter unavailable, using the SDL renderer");
			setPresenter(false);
		}
	}
#endif

	if (!_presenter)
		_renderer = SDL_CreateRenderer(_window, nullptr);

	/* SDL3 creates renderers with vsync disabled, so presents tear. Enabling it
	 * makes SDL_RenderPresent block until the next refresh, which throttles the
	 * update path in updateSurface() to the display rate. */
	if (_renderer)
	{
		if (!SDL_SetRenderVSync(_renderer, vsync() ? 1 : SDL_RENDERER_VSYNC_DISABLED))
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "SDL_SetRenderVSync: %s", SDL_GetError());
	}

	std::ignore = resizeToScale();

	_monitor = query(_window, id, true);
}

SdlWindow::SdlWindow(SdlWindow&& other) noexcept
    : _presenter(std::move(other._presenter)), _presentPending(other._presentPending),
      _presentSlot(other._presentSlot), _presentPendingSince(other._presentPendingSince),
      _window(other._window), _renderer(other._renderer), _renderTarget(other._renderTarget),
      _gdiTexture(other._gdiTexture), _gdiTextureW(other._gdiTextureW),
      _gdiTextureH(other._gdiTextureH), _initialW(other._initialW), _initialH(other._initialH),
      _displayID(other._displayID), _offset_x(other._offset_x), _offset_y(other._offset_y),
      _monitor(other._monitor)
{
	other._window = nullptr;
	other._renderer = nullptr;
	other._renderTarget = nullptr;
	other._gdiTexture = nullptr;
}

SdlWindow::~SdlWindow()
{
	/* Before the window it draws to. */
	_presenter.reset();
	if (_gdiTexture)
		SDL_DestroyTexture(_gdiTexture);
	if (_renderTarget)
		SDL_DestroyTexture(_renderTarget);
	if (_renderer)
		SDL_DestroyRenderer(_renderer);
	if (_window)
		SDL_DestroyWindow(_window);
}

SDL_WindowID SdlWindow::id() const
{
	if (!_window)
		return 0;
	return SDL_GetWindowID(_window);
}

SDL_DisplayID SdlWindow::displayIndex() const
{
	if (!_window)
		return 0;
	return SDL_GetDisplayForWindow(_window);
}

SDL_Rect SdlWindow::rect() const
{
	return rect(_window);
}

SDL_Rect SdlWindow::bounds() const
{
	SDL_Rect rect = {};
	if (_window)
	{
		if (!SDL_GetWindowPosition(_window, &rect.x, &rect.y))
			return {};
		if (!SDL_GetWindowSize(_window, &rect.w, &rect.h))
			return {};
	}
	return rect;
}

SDL_Window* SdlWindow::window() const
{
	return _window;
}

SDL_Renderer* SdlWindow::renderer() const
{
	return _renderer;
}

Sint32 SdlWindow::offsetX() const
{
	return _offset_x;
}

void SdlWindow::setOffsetX(Sint32 x)
{
	_offset_x = x;
}

void SdlWindow::setOffsetY(Sint32 y)
{
	_offset_y = y;
}

Sint32 SdlWindow::offsetY() const
{
	return _offset_y;
}

rdpMonitor SdlWindow::monitor(bool isPrimary) const
{
	auto m = _monitor;
	if (isPrimary)
	{
		m.x = 0;
		m.y = 0;
	}
	return m;
}

void SdlWindow::setMonitor(rdpMonitor monitor)
{
	_monitor = monitor;
}

float SdlWindow::scale() const
{
	return SDL_GetWindowDisplayScale(_window);
}

SDL_DisplayOrientation SdlWindow::orientation() const
{
	const auto did = displayIndex();
	return SDL_GetCurrentDisplayOrientation(did);
}

bool SdlWindow::grabKeyboard(bool enable)
{
	if (!_window)
		return false;
	SDL_SetWindowKeyboardGrab(_window, enable);
	return true;
}

bool SdlWindow::grabMouse(bool enable)
{
	if (!_window)
		return false;
	SDL_SetWindowMouseGrab(_window, enable);
	return true;
}

void SdlWindow::setBordered(bool bordered)
{
	if (_window)
		SDL_SetWindowBordered(_window, bordered);
	std::ignore = SDL_SyncWindow(_window);
}

void SdlWindow::raise()
{
	SDL_RaiseWindow(_window);
	std::ignore = SDL_SyncWindow(_window);
}

void SdlWindow::resizeable(bool use)
{
	SDL_SetWindowResizable(_window, use);
	std::ignore = SDL_SyncWindow(_window);
}

void SdlWindow::fullscreen(bool enter, bool forceOriginalDisplay)
{
	if (enter && forceOriginalDisplay && _displayID != 0)
	{
		/* Move the window to the desired display. We should not wait
		 * for the window to be moved, because some backends can refuse
		 * the move. The intent of moving the window is enough for SDL
		 * to decide which display will be used for fullscreen. */
		SDL_Rect rect = {};
		std::ignore = SDL_GetDisplayBounds(_displayID, &rect);
		std::ignore = SDL_SetWindowPosition(_window, rect.x, rect.y);
	}
	std::ignore = SDL_SetWindowFullscreen(_window, enter);
	std::ignore = SDL_SyncWindow(_window);
}

void SdlWindow::minimize()
{
	SDL_MinimizeWindow(_window);
	std::ignore = SDL_SyncWindow(_window);
}

bool SdlWindow::resizeToScale()
{
	if (!_window || _initialW <= 0 || _initialH <= 0)
		return false;
	if ((SDL_GetWindowFlags(_window) & SDL_WINDOW_FULLSCREEN) != 0)
		return true;

	float pd = SDL_GetWindowPixelDensity(_window);
	if (pd <= 0.0f)
		pd = 1.0f;

	const int targetW = static_cast<int>(std::ceil(static_cast<float>(_initialW) / pd));
	const int targetH = static_cast<int>(std::ceil(static_cast<float>(_initialH) / pd));

	int curW = 0;
	int curH = 0;
	if (!SDL_GetWindowSize(_window, &curW, &curH))
		return false;

	if (curW == targetW && curH == targetH)
		return true;

	return resize({ targetW, targetH });
}

bool SdlWindow::resize(const SDL_Point& size)
{
	return SDL_SetWindowSize(_window, size.x, size.y);
}

void SdlWindow::ensureRenderTarget()
{
	if (!_renderer)
		return;

	int w = 0;
	int h = 0;
	SDL_GetWindowSizeInPixels(_window, &w, &h);
	if (w <= 0 || h <= 0)
		return;

	/* Recreate if missing or if window size changed */
	if (_renderTarget)
	{
		float tw = 0;
		float th = 0;
		if (!SDL_GetTextureSize(_renderTarget, &tw, &th))
			return;
		if (static_cast<int>(tw) == w && static_cast<int>(th) == h)
			return;

		SDL_LogInfo(SDL_LOG_CATEGORY_RENDER, "render target %dx%d -> %dx%d", static_cast<int>(tw),
		            static_cast<int>(th), w, h);
		SDL_DestroyTexture(_renderTarget);
		/* Must not stay set: if the create below fails we would keep using a
		 * destroyed texture. */
		_renderTarget = nullptr;
	}

	_renderTarget =
	    SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_TARGET, w, h);
	if (!_renderTarget)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateTexture (render target): %s",
		             SDL_GetError());
		return;
	}
	/* Verbatim, never blended: a transparent window's alpha<0xFF regions would render black. */
	std::ignore = SDL_SetTextureBlendMode(_renderTarget, SDL_BLENDMODE_NONE);

	/* Clear once: transparent so windows never flash black before painting. */
	if (SDL_SetRenderTarget(_renderer, _renderTarget))
	{
		std::ignore = SDL_SetRenderDrawColor(_renderer, 0x00, 0x00, 0x00, 0x00);
		std::ignore = SDL_RenderClear(_renderer);
	}
}

bool SdlWindow::drawRect(SDL_Surface* surface, SDL_Point offset, const SDL_Rect& srcRect)
{
	WINPR_ASSERT(surface);
	SDL_Rect dstRect = { offset.x + srcRect.x, offset.y + srcRect.y, srcRect.w, srcRect.h };
	return blit(surface, srcRect, dstRect);
}

bool SdlWindow::presenterUpload(SDL_Surface* surface, SDL_Point offset, const SDL_FPoint& scale,
                                const std::vector<SDL_Rect>& rects)
{
#if defined(_WIN32)
	WINPR_ASSERT(surface);
	_presenter->setMapping(offset, scale);

	const auto start = SDL_GetTicksNS();
	Uint64 bytes = 0;
	if (!_presenter->upload(surface, rects, &bytes))
		return false;
	const auto now = SDL_GetTicksNS();
	addStat(STAT_UPLOAD, now - start);

	if (showStats())
	{
		_statsBlits += rects.empty() ? 1 : rects.size();
		_statsBytes += bytes;
	}
	_gdiTextureW = surface->w;
	_gdiTextureH = surface->h;

	if (!_presentPending)
		_presentPendingSince = now;
	_presentPending = true;
	return true;
#else
	return false;
#endif
}

bool SdlWindow::drawRects(SDL_Surface* surface, SDL_Point offset,
                          const std::vector<SDL_Rect>& rects)
{
	if (_presenter)
		return presenterUpload(surface, offset, { 1.0f, 1.0f }, rects);
	if (rects.empty())
	{
		return drawRect(surface, offset, { 0, 0, surface->w, surface->h });
	}
	for (auto& srcRect : rects)
	{
		if (!drawRect(surface, offset, srcRect))
			return false;
	}
	return true;
}

bool SdlWindow::drawScaledRect(SDL_Surface* surface, const SDL_FPoint& scale,
                               const SDL_Rect& srcRect)
{
	SDL_Rect dstRect = {};
	float ix = 0.0f;
	float iy = 0.0f;
	const auto modx = std::modf(static_cast<float>(srcRect.x) * scale.x, &ix);
	const auto mody = std::modf(static_cast<float>(srcRect.y) * scale.y, &iy);
	auto sw = std::ceil(static_cast<float>(srcRect.w) * scale.x) + std::ceil(modx);
	auto sh = std::ceil(static_cast<float>(srcRect.h) * scale.y) + std::ceil(mody);
	dstRect.x = static_cast<Sint32>(ix);
	dstRect.w = static_cast<Sint32>(sw);
	dstRect.y = static_cast<Sint32>(iy);
	dstRect.h = static_cast<Sint32>(sh);
	return blit(surface, srcRect, dstRect);
}

bool SdlWindow::drawScaledRects(SDL_Surface* surface, const SDL_FPoint& scale,
                                const std::vector<SDL_Rect>& rects)
{
	if (_presenter)
		return presenterUpload(surface, { 0, 0 }, scale, rects);
	if (rects.empty())
	{
		return drawScaledRect(surface, scale, { 0, 0, surface->w, surface->h });
	}
	for (const auto& srcRect : rects)
	{
		if (!drawScaledRect(surface, scale, srcRect))
			return false;
	}
	return true;
}

bool SdlWindow::fill(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	/* The presenter clears around the desktop on every present, and a window
	 * surface would fight with its swap chain. */
	if (_presenter)
		return true;
	if (_renderer)
	{
		ensureRenderTarget();
		if (!SDL_SetRenderTarget(_renderer, _renderTarget))
			return false;
		if (!SDL_SetRenderDrawColor(_renderer, r, g, b, a))
			return false;
		return SDL_RenderClear(_renderer);
	}
	return fill(_window, r, g, b, a);
}

bool SdlWindow::fill(SDL_Window* window, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	auto surface = SDL_GetWindowSurface(window);
	if (!surface)
		return false;
	SDL_Rect rect = { 0, 0, surface->w, surface->h };
	auto color = SDL_MapSurfaceRGBA(surface, r, g, b, a);

	return SDL_FillSurfaceRect(surface, &rect, color);
}

rdpMonitor SdlWindow::query(SDL_Window* window, SDL_DisplayID id, bool forceAsPrimary)
{
	if (!window)
		return {};

	const auto& r = rect(window, forceAsPrimary);
	const float factor = SDL_GetWindowDisplayScale(window);
	const float dpi = std::roundf(factor * 100.0f);

	WINPR_ASSERT(r.w > 0);
	WINPR_ASSERT(r.h > 0);

	const auto primary = SDL_GetPrimaryDisplay();
	const auto orientation = SDL_GetCurrentDisplayOrientation(id);
	const auto rdp_orientation = sdl::utils::orientaion_to_rdp(orientation);

	rdpMonitor monitor{};
	monitor.orig_screen = id;
	monitor.x = r.x;
	monitor.y = r.y;
	monitor.width = r.w;
	monitor.height = r.h;
	monitor.is_primary = forceAsPrimary || (id == primary);
	monitor.attributes.desktopScaleFactor = static_cast<UINT32>(dpi);
	monitor.attributes.deviceScaleFactor = 100;
	monitor.attributes.orientation = rdp_orientation;
	monitor.attributes.physicalWidth = WINPR_ASSERTING_INT_CAST(uint32_t, r.w);
	monitor.attributes.physicalHeight = WINPR_ASSERTING_INT_CAST(uint32_t, r.h);

	const auto cat = SDL_LOG_CATEGORY_APPLICATION;
	SDL_LogDebug(cat, "monitor.orig_screen                   %" PRIu32, monitor.orig_screen);
	SDL_LogDebug(cat, "monitor.x                             %" PRId32, monitor.x);
	SDL_LogDebug(cat, "monitor.y                             %" PRId32, monitor.y);
	SDL_LogDebug(cat, "monitor.width                         %" PRId32, monitor.width);
	SDL_LogDebug(cat, "monitor.height                        %" PRId32, monitor.height);
	SDL_LogDebug(cat, "monitor.is_primary                    %" PRIu32, monitor.is_primary);
	SDL_LogDebug(cat, "monitor.attributes.desktopScaleFactor %" PRIu32,
	             monitor.attributes.desktopScaleFactor);
	SDL_LogDebug(cat, "monitor.attributes.deviceScaleFactor  %" PRIu32,
	             monitor.attributes.deviceScaleFactor);
	SDL_LogDebug(cat, "monitor.attributes.orientation        %s",
	             freerdp_desktop_rotation_flags_to_string(monitor.attributes.orientation));
	SDL_LogDebug(cat, "monitor.attributes.physicalWidth      %" PRIu32,
	             monitor.attributes.physicalWidth);
	SDL_LogDebug(cat, "monitor.attributes.physicalHeight     %" PRIu32,
	             monitor.attributes.physicalHeight);
	return monitor;
}

SDL_Rect SdlWindow::rect(SDL_Window* window, bool forceAsPrimary)
{
	SDL_Rect rect = {};
	if (!window)
		return {};

	if (!forceAsPrimary)
	{
		if (!SDL_GetWindowPosition(window, &rect.x, &rect.y))
			return {};
	}

	if (!SDL_GetWindowSizeInPixels(window, &rect.w, &rect.h))
		return {};

	const auto flags = SDL_GetWindowFlags(window);
	const auto mask = SDL_WINDOW_FULLSCREEN;
	const auto fs = (flags & mask) == mask;
	if (tryFallback(fs))
	{
		/* On wlroots compositors (Sway, river, etc.), windows that are hidden/unmapped
		 * don't get their actual display dimensions. The dummy window returns its creation size
		 * (64x64) instead of the display size. This causes validation errors since we require >=
		 * 200px. Workaround: If we got dimensions that are too small, query the display directly.
		 */

		const auto displayID = SDL_GetDisplayForWindow(window);
		SDL_Rect displayBounds = {};
		if (SDL_GetDisplayBounds(displayID, &displayBounds))
		{
			if (forceAsPrimary)
			{
				rect.x = 0;
				rect.y = 0;
			}
			else
			{
				/* The dummy window is centered on the display by the compositor, so its
				 * position is not the display origin. Use the display bounds as for w/h. */
				rect.x = displayBounds.x;
				rect.y = displayBounds.y;
			}
			rect.w = displayBounds.w;
			rect.h = displayBounds.h;

			const float contentScale = SDL_GetDisplayContentScale(displayID);
			if (contentScale > 1.0f)
			{
				const auto fw = static_cast<float>(rect.w);
				const auto fh = static_cast<float>(rect.h);
				rect.w = static_cast<int>(std::roundf(fw * contentScale));
				rect.h = static_cast<int>(std::roundf(fh * contentScale));
			}
		}
	}

	return rect;
}

SdlWindow::HighDPIMode SdlWindow::isHighDPIWindowsMode(SDL_Window* window)
{
	if (!window)
		return MODE_INVALID;

	const auto id = SDL_GetDisplayForWindow(window);
	if (id == 0)
		return MODE_INVALID;

	const auto cs = SDL_GetDisplayContentScale(id);
	const auto ds = SDL_GetWindowDisplayScale(window);
	const auto pd = SDL_GetWindowPixelDensity(window);

	/* mac os x style, but no HighDPI display */
	if ((cs == 1.0f) && (ds == 1.0f) && (pd == 1.0f))
		return MODE_NONE;

	/* mac os x style HighDPI */
	if ((cs == 1.0f) && (ds > 1.0f) && (pd > 1.0f))
		return MODE_MACOS;

	/* rest is windows style */
	return MODE_WINDOWS;
}

/* Lazily create or recreate the persistent streaming GDI texture to match `surface`. */
bool SdlWindow::ensureGdiTexture(SDL_Surface* surface)
{
	if (_gdiTexture && (_gdiTextureW == surface->w) && (_gdiTextureH == surface->h))
		return true;
	if (_gdiTexture)
		SDL_DestroyTexture(_gdiTexture);
	_gdiTexture = SDL_CreateTexture(_renderer, surface->format, SDL_TEXTUREACCESS_STREAMING,
	                                surface->w, surface->h);
	if (!_gdiTexture)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateTexture: %s", SDL_GetError());
		return false;
	}
	std::ignore = SDL_SetTextureBlendMode(_gdiTexture, SDL_BLENDMODE_NONE);
	_gdiTextureW = surface->w;
	_gdiTextureH = surface->h;
	return true;
}

bool SdlWindow::blit(SDL_Surface* surface, const SDL_Rect& srcRect, SDL_Rect& dstRect)
{
	if (!_renderer || !surface)
		return false;

	/* Before rendering into it, not after: updateSurface() used to be the only
	 * caller, so a missing or stale-sized target meant this blit went to the
	 * window backbuffer instead and was then overwritten by the target's older
	 * contents -- one frame behind, every frame. */
	ensureRenderTarget();

	if (!ensureGdiTexture(surface))
		return false;

	/* Upload only the dirty region */
	const auto* details = SDL_GetPixelFormatDetails(surface->format);
	const int bpp = details ? details->bytes_per_pixel : 4;
	const auto* pixels = static_cast<const uint8_t*>(surface->pixels) +
	                     (1ll * srcRect.y * surface->pitch) + (1ll * srcRect.x * bpp);
	const auto uploadStart = SDL_GetTicksNS();
	if (!SDL_UpdateTexture(_gdiTexture, &srcRect, pixels, surface->pitch))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_UpdateTexture: %s", SDL_GetError());
		return false;
	}
	const auto renderStart = SDL_GetTicksNS();
	addStat(STAT_UPLOAD, renderStart - uploadStart);

	if (showStats())
	{
		/* The dirty region, not the whole surface: this is what was actually
		 * handed over, so it tracks the incremental path rather than the
		 * framebuffer size. */
		_statsBlits++;
		_statsBytes += 1ull * srcRect.w * srcRect.h * static_cast<unsigned>(bpp);
	}

	/* Render onto persistent render target to accumulate dirty rects */
	if (!SDL_SetRenderTarget(_renderer, _renderTarget))
		return false;

	SDL_FRect fsrc = { static_cast<float>(srcRect.x), static_cast<float>(srcRect.y),
		               static_cast<float>(srcRect.w), static_cast<float>(srcRect.h) };
	SDL_FRect fdst = { static_cast<float>(dstRect.x), static_cast<float>(dstRect.y),
		               static_cast<float>(dstRect.w), static_cast<float>(dstRect.h) };
	if (!SDL_RenderTexture(_renderer, _gdiTexture, &fsrc, &fdst))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_RenderTexture: %s", SDL_GetError());
		return false;
	}

	if (showStats())
	{
		/* SDL batches draw calls; without the flush the copy would be billed
		 * to whatever flushes next. */
		(void)SDL_FlushRenderer(_renderer);
		addStat(STAT_RENDER, SDL_GetTicksNS() - renderStart);
	}

	if (showDamage())
		_damageRects.push_back(dstRect);

	return true;
}

/* Set from the command line before the first window is created, read from the
 * SDL thread only. */
static bool s_showDamage = false;
static bool s_showStats = false;
static float s_statsScale = 2.0f;
static const float s_statsLineHeight = static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) + 2.0f;
static bool s_vsync = true;
#if defined(_WIN32)
static bool s_presenter = true;
#else
static bool s_presenter = false;
#endif
static FILE* s_statsLog = nullptr; /* never closed, lives as long as the process */

/* Stage timings. s_frame collects the frame being drawn and is folded into
 * s_stages when it is presented. SDL thread only, apart from s_framesIn. */
struct StageStat
{
	Uint64 sum = 0;
	Uint64 max = 0;
	Uint64 count = 0;
};
static std::array<StageStat, SdlWindow::STAT_COUNT> s_stages;
static std::array<Uint64, SdlWindow::STAT_COUNT> s_frame;
static std::array<bool, SdlWindow::STAT_COUNT> s_frameHas;
static Uint64 s_frameQueuedAt = 0;
static std::atomic<Uint64> s_framesIn{ 0 };

void SdlWindow::setVSync(bool enable)
{
	s_vsync = enable;
}

bool SdlWindow::vsync()
{
	return s_vsync;
}

void SdlWindow::setPresenter(bool enable)
{
#if defined(_WIN32)
	s_presenter = enable;
#else
	(void)enable;
#endif
}

bool SdlWindow::presenterEnabled()
{
	return s_presenter;
}

bool SdlWindow::hasPresenter() const
{
	return _presenter != nullptr;
}

void* SdlWindow::presentHandle() const
{
#if defined(_WIN32)
	if (_presenter)
		return _presenter->waitHandle();
#endif
	return nullptr;
}

bool SdlWindow::presentPending() const
{
	return _presentPending;
}

bool SdlWindow::hasPresentSlot() const
{
	return _presentSlot;
}

void SdlWindow::presentSlot()
{
	_presentSlot = true;
}

void SdlWindow::presenterPresent()
{
#if defined(_WIN32)
	const auto drawStart = SDL_GetTicksNS();
	addStat(STAT_SWAPWAIT, drawStart - _presentPendingSince);

	_presentPending = false;
	_presentSlot = false;
	if (!_presenter->draw())
		return;
	const auto presentStart = SDL_GetTicksNS();
	addStat(STAT_COMPOSITE, presentStart - drawStart);

	if (!_presenter->present())
		return;

	if (showStats())
	{
		const auto now = SDL_GetTicksNS();
		addStat(STAT_PRESENT, now - presentStart);
		if (s_frameQueuedAt != 0)
			addStat(STAT_TOTAL, now - s_frameQueuedAt);
		commitFrameStats();
		_statsFrames++;
		/* Shows from the next present on. */
		if (updateStats())
			updatePresenterOverlay();
	}
#endif
}

bool SdlWindow::takeRedrawRequest()
{
#if defined(_WIN32)
	if (_presenter)
		return _presenter->takeRedrawRequest();
#endif
	return false;
}

bool SdlWindow::setStatsLog(const char* path)
{
	s_statsLog = fopen(path, "a");
	if (!s_statsLog)
		return false;
	s_showStats = true;
	return true;
}

void SdlWindow::addStat(StatStage stage, Uint64 ns)
{
	if (!s_showStats)
		return;
	s_frame[stage] += ns;
	s_frameHas[stage] = true;
}

void SdlWindow::setFrameQueuedAt(Uint64 ns)
{
	s_frameQueuedAt = ns;
}

void SdlWindow::countFrameIn()
{
	if (s_showStats)
		s_framesIn++;
}

void SdlWindow::commitFrameStats()
{
	for (size_t x = 0; x < s_stages.size(); x++)
	{
		if (!s_frameHas[x])
			continue;
		auto& stage = s_stages[x];
		stage.sum += s_frame[x];
		stage.max = std::max(stage.max, s_frame[x]);
		stage.count++;
	}
	s_frame.fill(0);
	s_frameHas.fill(false);
	s_frameQueuedAt = 0;
}

void SdlWindow::setShowDamage(bool enable)
{
	s_showDamage = enable;
}

void SdlWindow::setShowStats(bool enable, float scale)
{
	s_showStats = enable;
	if (scale > 0.0f)
		s_statsScale = scale;
}

bool SdlWindow::showDamage()
{
	return s_showDamage;
}

bool SdlWindow::showStats()
{
	return s_showStats;
}

float SdlWindow::statsScale()
{
	return s_statsScale;
}

void SdlWindow::renderStats()
{
	(void)updateStats();
	if (_statsText.empty())
		return;

	const auto scale = statsScale();
	float sx = 1.0f;
	float sy = 1.0f;
	(void)SDL_GetRenderScale(_renderer, &sx, &sy);
	if (!SDL_SetRenderScale(_renderer, scale, scale))
		return;
	renderStatsText(_renderer);
	(void)SDL_SetRenderScale(_renderer, sx, sy);
}

SDL_FRect SdlWindow::statsBox() const
{
	size_t longest = 0;
	for (const auto& text : _statsText)
		longest = std::max(longest, text.size());
	const auto chars = static_cast<float>(longest);
	return { 2.0f, 2.0f, chars * static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) + 8.0f,
		     static_cast<float>(_statsText.size()) * s_statsLineHeight + 6.0f };
}

void SdlWindow::updatePresenterOverlay()
{
#if defined(_WIN32)
	if (_statsText.empty())
		return;

	/* The presenter has no text drawing of its own, so the overlay is drawn
	 * into a surface with SDL's software renderer and handed over as a
	 * texture. Only when the numbers change, i.e. twice a second. */
	const auto scale = statsScale();
	const auto box = statsBox();
	const auto w = static_cast<int>(std::ceil((box.x + box.w) * scale));
	const auto h = static_cast<int>(std::ceil((box.y + box.h) * scale));

	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
	    SDL_CreateSurface(w, h, SDL_PIXELFORMAT_BGRA32), SDL_DestroySurface);
	if (!surface)
		return;
	std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
	    SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
	if (!renderer)
		return;

	if (!SDL_SetRenderDrawColor(renderer.get(), 0x00, 0x00, 0x00, 0x00) ||
	    !SDL_RenderClear(renderer.get()) || !SDL_SetRenderScale(renderer.get(), scale, scale))
		return;
	renderStatsText(renderer.get());
	if (!SDL_RenderPresent(renderer.get()))
		return;

	(void)_presenter->setOverlay(surface.get());
#endif
}

bool SdlWindow::updateStats()
{
	const auto now = SDL_GetTicksNS();
	if (_statsWindowStart == 0)
		_statsWindowStart = now;
	bool changed = false;

	/* Recompute at most twice a second: at frame rate the numbers flicker too
	 * fast to read, and the averaging is what makes them meaningful. */
	const auto elapsed = now - _statsWindowStart;
	if (elapsed >= 500000000ull)
	{
		const auto secs = static_cast<double>(elapsed) / 1000000000.0;
		const auto fps = static_cast<double>(_statsFrames) / secs;
		const auto mbps = static_cast<double>(_statsBytes) / secs / (1024.0 * 1024.0);
		const auto bpf = (_statsFrames > 0) ? (_statsBlits / _statsFrames) : _statsBlits;

		const auto in = static_cast<double>(s_framesIn.exchange(0)) / secs;
		int vsync = 0;
		if (_renderer)
			(void)SDL_GetRenderVSync(_renderer, &vsync);
		const char* name = _presenter ? "presenter" : SDL_GetRendererName(_renderer);
		if (_presenter)
			vsync = s_vsync ? 1 : 0;

		char buffer[160] = { 0 };
		_statsText.clear();
		(void)SDL_snprintf(buffer, sizeof(buffer),
		                   "%5.1f fps (%5.1f in)  %7.2f MiB/s  %llu blits/frame  %dx%d  %s%s",
		                   fps, in, mbps, static_cast<unsigned long long>(bpf), _gdiTextureW,
		                   _gdiTextureH, name ? name : "?", (vsync != 0) ? " vsync" : "");
		_statsText.emplace_back(buffer);

		/* avg/max in ms per stage, two lines so it fits a 1280 wide window. */
		static const std::array<const char*, STAT_COUNT> names = {
			"wait", "lock", "upload", "render", "ack", "swapwait", "composite", "present", "total"
		};
		std::string line = "ms avg/max";
		for (size_t x = 0; x < s_stages.size(); x++)
		{
			const auto& stage = s_stages[x];
			const auto avg = (stage.count > 0) ? static_cast<double>(stage.sum) /
			                                         static_cast<double>(stage.count) / 1e6
			                                   : 0.0;
			(void)SDL_snprintf(buffer, sizeof(buffer), "  %s %.2f/%.2f", names[x], avg,
			                   static_cast<double>(stage.max) / 1e6);
			line += buffer;
			if (x == STAT_RENDER)
			{
				_statsText.push_back(line);
				line = "          ";
			}
		}
		_statsText.push_back(line);
		s_stages.fill({});

		/* The overlay can't be copied out of, so the same numbers go to stderr
		 * or the stats log. */
		auto out = s_statsLog ? s_statsLog : stderr;
		for (const auto& text : _statsText)
			(void)fprintf(out, "[stats] %s\n", text.c_str());
		(void)fflush(out);

		_statsWindowStart = now;
		_statsFrames = 0;
		_statsBlits = 0;
		_statsBytes = 0;
		changed = true;
	}
	return changed;
}

void SdlWindow::renderStatsText(SDL_Renderer* renderer)
{
	/* Backing box, so the text stays legible over arbitrary desktop content.
	 * The caller has set the text scale; coordinates are in scaled units. */
	const auto box = statsBox();
	SDL_BlendMode blend = SDL_BLENDMODE_NONE;
	(void)SDL_GetRenderDrawBlendMode(renderer, &blend);

	if (SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND) &&
	    SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0x00, 0xc0))
		(void)SDL_RenderFillRect(renderer, &box);

	if (SDL_SetRenderDrawColor(renderer, 0x00, 0xff, 0x00, 0xff))
	{
		auto y = box.y + 4.0f;
		for (const auto& text : _statsText)
		{
			(void)SDL_RenderDebugText(renderer, box.x + 4.0f, y, text.c_str());
			y += s_statsLineHeight;
		}
	}

	(void)SDL_SetRenderDrawBlendMode(renderer, blend);
}

void SdlWindow::updateSurface()
{
	/* Taken by move so the rects are dropped even if a step below bails out;
	 * otherwise a failed present would leave them to be drawn again next
	 * frame, which is exactly what this is meant to show is not happening. */
	const auto damage = std::move(_damageRects);
	_damageRects.clear();

	if (_presenter)
	{
		presenterPresent();
		return;
	}

	if (!_renderer)
		return;

	/* blit() creates the target before rendering into it, so normally there is
	 * nothing to do here. Only create it if it is missing entirely -- calling
	 * ensureRenderTarget() unconditionally would destroy and recreate it on a
	 * size change, discarding exactly the content we are about to present. */
	if (!_renderTarget)
	{
		ensureRenderTarget();
		if (!_renderTarget)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "updateSurface: no render target");
			return;
		}
	}

	const auto compositeStart = SDL_GetTicksNS();

	/* Copy accumulated render target to screen and present. Each step is
	 * logged on failure: these used to return silently, which looks exactly
	 * like a healthy draw path that never reaches the screen. */
	if (!SDL_SetRenderTarget(_renderer, nullptr))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "updateSurface: SDL_SetRenderTarget: %s",
		             SDL_GetError());
		return;
	}
	if (!SDL_RenderTexture(_renderer, _renderTarget, nullptr, nullptr))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "updateSurface: SDL_RenderTexture: %s",
		             SDL_GetError());
		return;
	}

	/* Tint what was updated this frame.
	 *
	 * Deliberately drawn here, onto the backbuffer, rather than into
	 * _renderTarget: the target accumulates dirty rects across frames, so a
	 * tint drawn there would never be painted over and the whole screen would
	 * slowly turn pink. On the backbuffer it lives for exactly this present,
	 * and the next frame starts again from a clean copy of the target.
	 *
	 * The target is created at the window's pixel size and stretched over the
	 * whole output above, so the blit coordinates carry over unchanged. */
	if (!damage.empty())
	{
		std::vector<SDL_FRect> rects;
		rects.reserve(damage.size());
		for (const auto& r : damage)
			rects.push_back({ static_cast<float>(r.x), static_cast<float>(r.y),
			                  static_cast<float>(r.w), static_cast<float>(r.h) });

		if (!SDL_SetRenderDrawColor(_renderer, 0xFF, 0x00, 0x80, 0xff) ||
		    !SDL_RenderRects(_renderer, rects.data(), static_cast<int>(rects.size())))
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "updateSurface: damage overlay: %s",
			             SDL_GetError());
		}
	}

	/* Drawn onto the backbuffer for the same reason as the damage tint: the
	 * render target accumulates across frames, so an overlay there would smear. */
	if (showStats())
	{
		_statsFrames++;
		renderStats();
	}

	Uint64 presentStart = 0;
	if (showStats())
	{
		(void)SDL_FlushRenderer(_renderer);
		presentStart = SDL_GetTicksNS();
		addStat(STAT_COMPOSITE, presentStart - compositeStart);
	}

	if (!SDL_RenderPresent(_renderer))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "updateSurface: SDL_RenderPresent: %s",
		             SDL_GetError());
		return;
	}

	if (showStats())
	{
		const auto now = SDL_GetTicksNS();
		addStat(STAT_PRESENT, now - presentStart);
		if (s_frameQueuedAt != 0)
			addStat(STAT_TOTAL, now - s_frameQueuedAt);
		commitFrameStats();
	}
}

bool SdlWindow::paintResizeFrame(SDL_Surface* surface, SDL_Point off, bool contentChanged,
                                 const SDL_Rect& inset, bool fillRevealed, bool dashedBorder)
{
	if (!_renderer || !surface)
		return false;
	ensureRenderTarget();

	const int prevW = _gdiTextureW;
	const int prevH = _gdiTextureH;
	if (!ensureGdiTexture(surface))
		return false;
	/* A recreated texture is empty, so upload even when the content did not change. */
	const bool recreated = (_gdiTextureW != prevW) || (_gdiTextureH != prevH);
	if ((contentChanged || recreated) &&
	    !SDL_UpdateTexture(_gdiTexture, nullptr, surface->pixels, surface->pitch))
		return false;

	if (!SDL_SetRenderTarget(_renderer, _renderTarget))
		return false;

	int ww = 0;
	int wh = 0;
	SDL_GetWindowSizeInPixels(_window, &ww, &wh);
	/* The visible frame; the ring outside it (the resize band) stays fully transparent. */
	const SDL_FRect frame = { static_cast<float>(inset.x), static_cast<float>(inset.y),
		                      static_cast<float>(ww - inset.x - inset.w),
		                      static_cast<float>(wh - inset.y - inset.h) };

	/* Translucent fill in revealed area awaiting server frame. */
	constexpr Uint8 kFillAlpha = 0x80;
	std::ignore = SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_NONE);
	std::ignore = SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 0);
	std::ignore = SDL_RenderClear(_renderer);
	if (fillRevealed)
	{
		std::ignore = SDL_SetRenderDrawColor(_renderer, 0x2B, 0x2B, 0x2B, kFillAlpha);
		std::ignore = SDL_RenderFillRect(_renderer, &frame);
	}
	SDL_FRect fdst = { static_cast<float>(off.x), static_cast<float>(off.y),
		               static_cast<float>(surface->w), static_cast<float>(surface->h) };
	/* Anchored frame clipped to visible bounds. */
	const SDL_Rect clip = { inset.x, inset.y, ww - inset.x - inset.w, wh - inset.y - inset.h };
	std::ignore = SDL_SetRenderClipRect(_renderer, &clip);
	std::ignore = SDL_RenderTexture(_renderer, _gdiTexture, nullptr, &fdst);
	std::ignore = SDL_SetRenderClipRect(_renderer, nullptr);

	/* Dashed border indicating pending resize target. */
	if (dashedBorder)
	{
		std::ignore = SDL_SetRenderDrawColor(_renderer, 0xC8, 0xC8, 0xC8, 0xFF);
		const float dash = 8.0F;
		const float gap = 5.0F;
		const float fx2 = frame.x + frame.w;
		const float fy2 = frame.y + frame.h;
		const float lo = frame.y + 0.5F;
		const float by = fy2 - 0.5F;
		const float lx = frame.x + 0.5F;
		const float rx = fx2 - 0.5F;
		const float step = dash + gap;
		const auto steps = [step](float len)
		{ return (len <= 0.0F) ? 0 : static_cast<int>(std::ceil(len / step)); };
		for (int i = 0; i < steps(fx2 - frame.x); i++)
		{
			const float x = frame.x + (static_cast<float>(i) * step);
			const float x2 = (x + dash < fx2) ? (x + dash) : rx;
			std::ignore = SDL_RenderLine(_renderer, x, lo, x2, lo);
			std::ignore = SDL_RenderLine(_renderer, x, by, x2, by);
		}
		for (int i = 0; i < steps(fy2 - frame.y); i++)
		{
			const float y = frame.y + (static_cast<float>(i) * step);
			const float y2 = (y + dash < fy2) ? (y + dash) : by;
			std::ignore = SDL_RenderLine(_renderer, lx, y, lx, y2);
			std::ignore = SDL_RenderLine(_renderer, rx, y, rx, y2);
		}
	}

	if (!SDL_SetRenderTarget(_renderer, nullptr))
		return false;
	std::ignore = SDL_RenderTexture(_renderer, _renderTarget, nullptr, nullptr);
	std::ignore = SDL_RenderPresent(_renderer);
	return true;
}

SdlWindow SdlWindow::create(SDL_DisplayID id, const std::string& title, Uint32 flags, Uint32 width,
                            Uint32 height, bool presenter)
{
	flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

	SDL_Rect rect = { static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(id)),
		              static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(id)), static_cast<int>(width),
		              static_cast<int>(height) };

	if ((flags & SDL_WINDOW_FULLSCREEN) != 0)
	{
		std::ignore = SDL_GetDisplayBounds(id, &rect);
	}

	SdlWindow window{ id, title, rect, flags, presenter };

	if ((flags & SDL_WINDOW_FULLSCREEN) != 0)
	{
		window.setOffsetX(rect.x);
		window.setOffsetY(rect.y);
	}

	return window;
}

SdlWindow SdlWindow::create(SDL_DisplayID id, const std::string& title, Uint32 flags,
                            const SDL_Rect& rect)
{
	return SdlWindow{ id, title, rect, flags };
}

/* Popup constructor: positioned relative to the parent origin. */
SdlWindow::SdlWindow(SDL_Window* parent, const SDL_Rect& rect, bool transparent, bool tooltip)
    : _initialW(rect.w), _initialH(rect.h)
{
	auto props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, parent);
	SDL_SetBooleanProperty(props,
	                       tooltip ? SDL_PROP_WINDOW_CREATE_TOOLTIP_BOOLEAN
	                               : SDL_PROP_WINDOW_CREATE_MENU_BOOLEAN,
	                       true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
	/* Transparent so menus' genuine per-pixel alpha (corners/shadow) isn't rendered black. */
	if (transparent)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, true);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, rect.x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, rect.y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, rect.w);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, rect.h);
	/* Hidden until first paint, like the app constructor. */
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);

	_window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);
	if (_window)
	{
		std::ignore = SDL_SyncWindow(_window);
		_renderer = SDL_CreateRenderer(_window, nullptr);
		_displayID = SDL_GetDisplayForWindow(_window);
	}
}

SdlWindow SdlWindow::createPopup(SDL_Window* parent, const SDL_Rect& rect, bool transparent,
                                 bool tooltip)
{
	return SdlWindow{ parent, rect, transparent, tooltip };
}

static SDL_Window* createDummy(SDL_DisplayID id)
{
	const auto x = SDL_WINDOWPOS_CENTERED_DISPLAY(id);
	const auto y = SDL_WINDOWPOS_CENTERED_DISPLAY(id);
	const int w = 64;
	const int h = 64;

	auto props = SDL_CreateProperties();
	std::stringstream ss;
	ss << "SdlWindow::query(" << id << ")";
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, ss.str().c_str());
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);

	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, false);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, false);

	auto window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);

	/* Workaround: we need to properly position the window on the correct monitor
	 * before going fullscreen. Otherwise we will get the primary monitor details.
	 */
	if (window)
	{
		SDL_Rect rect = {};
		std::ignore = SDL_GetDisplayBounds(id, &rect);
		std::ignore = SDL_SetWindowPosition(window, rect.x, rect.y);
		std::ignore = SDL_SetWindowFullscreen(window, true);
	}
	return window;
}

rdpMonitor SdlWindow::query(SDL_DisplayID id, bool forceAsPrimary)
{
	std::unique_ptr<SDL_Window, void (*)(SDL_Window*)> window(createDummy(id), SDL_DestroyWindow);
	if (!window)
		return {};

	std::unique_ptr<SDL_Renderer, void (*)(SDL_Renderer*)> renderer(
	    SDL_CreateRenderer(window.get(), nullptr), SDL_DestroyRenderer);

	if (!SDL_SyncWindow(window.get()))
		return {};

	SDL_Event event{};
	while (SDL_PollEvent(&event))
		;

	return query(window.get(), id, forceAsPrimary);
}

SDL_Rect SdlWindow::rect(SDL_DisplayID id, bool forceAsPrimary)
{
	std::unique_ptr<SDL_Window, void (*)(SDL_Window*)> window(createDummy(id), SDL_DestroyWindow);
	if (!window)
		return {};

	std::unique_ptr<SDL_Renderer, void (*)(SDL_Renderer*)> renderer(
	    SDL_CreateRenderer(window.get(), nullptr), SDL_DestroyRenderer);

	if (!SDL_SyncWindow(window.get()))
		return {};

	SDL_Event event{};
	while (SDL_PollEvent(&event))
		;

	return rect(window.get(), forceAsPrimary);
}

bool SdlWindow::tryFallback(bool isFullscreen)
{
	/* If we define a custom env variable to use the wlroots hack
	 * then enable/disable according to this setting only.
	 */
	const auto wlroots_hack = SDL_getenv("FREERDP_WLROOTS_HACK");
	if (wlroots_hack != nullptr)
	{
		const auto enabled = strcmp(wlroots_hack, "0") != 0;
		if (strcmp(wlroots_hack, "force") == 0)
			isFullscreen = true;
		return enabled && isFullscreen;
	}

	const auto platform = SDL_GetPlatform();
	if ((platform == nullptr) || (strcmp(platform, "Linux") != 0))
		return false;

	const auto driver = SDL_GetCurrentVideoDriver();
	if ((driver == nullptr) || (strcmp(driver, "wayland") != 0))
		return false;

	/* Check XDG_SESSION_DESKTOP and XDG_CURRENT_DESKTOP for wlroots-based
	 * compositors. The original check only matched Sway, but other wlroots
	 * compositors (Hyprland, river, etc.) have the same dummy-window sizing
	 * behavior where hidden/unmapped windows return 64x64 instead of the
	 * display size. Use strstr for substring matching since XDG_CURRENT_DESKTOP
	 * can be a colon-separated list (e.g. "sway:wlroots", "Hyprland").
	 */
	auto isWlrootsCompositor = [](const char* value) -> bool
	{
		if (!value)
			return false;
		if (strstr(value, "sway") || strstr(value, "Sway") || strstr(value, "Hyprland") ||
		    strstr(value, "hyprland") || strstr(value, "river") || strstr(value, "wlroots"))
			return true;
		return false;
	};

	const auto xdg_session = SDL_getenv("XDG_SESSION_DESKTOP");
	if (isWlrootsCompositor(xdg_session))
		return isFullscreen;

	const auto xdg_desktop = SDL_getenv("XDG_CURRENT_DESKTOP");
	if (isWlrootsCompositor(xdg_desktop))
		return isFullscreen;

	return false;
}
