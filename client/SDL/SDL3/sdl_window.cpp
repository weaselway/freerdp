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
#include "sdl_win32_vblank.hpp"
#else
/* Never created, but the window holds pointers to them. */
class SdlD3D11Presenter
{
};
class SdlWin32VBlank
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
	if (presenter && presenterEnabled())
	{
		auto hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(_window),
		                                   SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
		_presenter = SdlD3D11Presenter::create(hwnd, vsync(), zeroCopyEnabled());
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
      _presentPendingQpc(other._presentPendingQpc), _vblank(std::move(other._vblank)),
      _timingSlackUs(std::move(other._timingSlackUs)), _timingSince(other._timingSince),
      _window(other._window), _renderer(other._renderer), _renderTarget(other._renderTarget),
      _gdiTexture(other._gdiTexture), _gdiTextureW(other._gdiTextureW),
      _gdiTextureH(other._gdiTextureH), _initialW(other._initialW), _initialH(other._initialH),
      _displayID(other._displayID), _offset_x(other._offset_x), _offset_y(other._offset_y),
      _monitor(other._monitor), _monitorIndex(other._monitorIndex.load()),
      _notificationText(std::move(other._notificationText)),
      _notification(std::move(other._notification)),
      _notificationChanged(other._notificationChanged)
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
	_vblank.reset();
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

UINT16 SdlWindow::monitorIndex() const
{
	return _monitorIndex;
}

void SdlWindow::setMonitorIndex(UINT16 index)
{
	_monitorIndex = index;
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
	/* No SDL_SyncWindow: this runs for every window event, and on macOS each one waited for SDL's
	 * sync timeout (2.5 s) while a full screen change was pending. SDL ignores an unchanged border
	 * anyway. */
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
	SdlD3D11Presenter::UploadInfo info;
	if (!_presenter->upload(surface, rects, &info))
		return false;
	const auto now = SDL_GetTicksNS();
	addStat(STAT_UPLOAD, now - start);

	if (showStats())
	{
		_statsBlits += rects.empty() ? 1 : rects.size();
		_statsBytes += info.bytes;
		if (info.bytes > 0)
		{
			auto& path = _statsUploads[info.zeroCopy ? 1 : 0];
			path.count++;
			path.bytes += info.bytes;
			path.busyNS += now - start;
		}
	}
	_gdiTextureW = surface->w;
	_gdiTextureH = surface->h;

	/* In window pixels, where the presenter draws them. */
	if (showDamage())
	{
		const auto toWindow = [&](const SDL_Rect& r)
		{
			const auto x0 = static_cast<float>(r.x) * scale.x;
			const auto y0 = static_cast<float>(r.y) * scale.y;
			const auto x1 = static_cast<float>(r.x + r.w) * scale.x;
			const auto y1 = static_cast<float>(r.y + r.h) * scale.y;
			_damageRects.push_back({ offset.x + static_cast<int>(std::floor(x0)),
			                         offset.y + static_cast<int>(std::floor(y0)),
			                         static_cast<int>(std::ceil(x1) - std::floor(x0)),
			                         static_cast<int>(std::ceil(y1) - std::floor(y0)) });
		};
		if (rects.empty())
			toWindow({ 0, 0, surface->w, surface->h });
		for (const auto& r : rects)
			toWindow(r);
	}

	if (!_presentPending)
	{
		_presentPendingSince = now;
		LARGE_INTEGER qpc = {};
		(void)QueryPerformanceCounter(&qpc);
		_presentPendingQpc = qpc.QuadPart;
	}
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

static rdpMonitor makeMonitor(SDL_DisplayID id, const SDL_Rect& r, float factor,
                              bool forceAsPrimary)
{
	const float dpi = std::roundf(factor * 100.0f);

	/* A compositor without an output can report a display of 0x0 (Xwayland in a headless
	 * mutter), the caller skips such a display. */
	if ((r.w <= 0) || (r.h <= 0))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "display %" PRIu32 ": invalid size %dx%d", id, r.w,
		            r.h);
		return {};
	}

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
	/* The physical size is in millimetres and ignored outside 10 to 10000 ([MS-RDPBCGR]
	 * 2.2.1.3.9.1, [MS-RDPEDISP] 2.2.2.2.1). SDL3 does not know a display's physical size, so
	 * leave it at 0 (not given) instead of sending the size in pixels as millimetres. */
	monitor.attributes.physicalWidth = 0;
	monitor.attributes.physicalHeight = 0;

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

rdpMonitor SdlWindow::query(SDL_Window* window, SDL_DisplayID id, bool forceAsPrimary)
{
	if (!window)
		return {};
	return makeMonitor(id, rect(window, forceAsPrimary), SDL_GetWindowDisplayScale(window),
	                   forceAsPrimary);
}

/* Dummy windows report wrong sizes on Wayland and X11, use the display. */
static rdpMonitor queryDisplay(SDL_DisplayID id, bool forceAsPrimary)
{
	SDL_Rect r = {};
	const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
	if (!mode || !SDL_GetDisplayBounds(id, &r))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "display %" PRIu32 ": %s", id, SDL_GetError());
		return {};
	}
	if (forceAsPrimary)
	{
		r.x = 0;
		r.y = 0;
	}
	r.w = static_cast<int>(std::roundf(static_cast<float>(r.w) * mode->pixel_density));
	r.h = static_cast<int>(std::roundf(static_cast<float>(r.h) * mode->pixel_density));
	return makeMonitor(id, r, mode->pixel_density * SDL_GetDisplayContentScale(id), forceAsPrimary);
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
static std::atomic<bool> s_showDamage{ false }; /* the hotkey toggles it */
static std::atomic<bool> s_showStats{ false }; /* the hotkey toggles it */
static float s_statsScale = 2.0f;
static const float s_statsLineHeight = static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) + 2.0f;
static bool s_vsync = true;
#if defined(_WIN32)
static bool s_presenter = true;
#else
static bool s_presenter = false;
#endif
static bool s_zeroCopy = true;
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

/* Per sampling window: each frame's latency (queued -> presented), and of
 * the frames whose deadline is known, how many missed it. */
static std::vector<float> s_latencyMs;
static Uint64 s_lateFrames = 0;
static Uint64 s_deadlineFrames = 0;

/* The last frames for the graphs, oldest at s_historyNext once full. */
struct FrameSample
{
	std::array<float, SdlWindow::STAT_COUNT> ms{}; /* stage times */
	float intervalMs = 0.0f; /* since the window's previous present, 0 for the first */
	bool late = false;
};
static std::array<FrameSample, 512> s_history;
static size_t s_historyNext = 0;
static size_t s_historyCount = 0;

/* Graph and legend colours per stage, 0xRRGGBB, and for late frames. */
static constexpr std::array<Uint32, SdlWindow::STAT_COUNT> s_stageColors = {
	0x64b5f6, 0xffb74d, 0x81c784, 0xf06292, 0xa1887f, 0xba68c8, 0xfff176, 0xbdbdbd, 0xe6e1e5
};
static constexpr Uint32 s_lateColor = 0xef5350;

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

void SdlWindow::setZeroCopy(bool enable)
{
	s_zeroCopy = enable;
}

bool SdlWindow::zeroCopyEnabled()
{
	return s_zeroCopy;
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

bool SdlWindow::copyPending() const
{
#if defined(_WIN32)
	if (_presenter)
		return _presenter->copyPending();
#endif
	return false;
}

void* SdlWindow::copyHandle() const
{
#if defined(_WIN32)
	if (_presenter)
		return _presenter->copyHandle();
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

void SdlWindow::presenterPresent(const std::vector<SDL_Rect>& damage)
{
#if defined(_WIN32)
	const auto drawStart = SDL_GetTicksNS();
	addStat(STAT_SWAPWAIT, drawStart - _presentPendingSince);

	_presentPending = false;
	_presentSlot = false;
	{
		std::unique_lock lock(_notificationLock);
		if (_notificationChanged && _presenter->setNotification(_notification.get()))
			_notificationChanged = false;
	}
	_presenter->setDamage(damage);
	if (!_presenter->draw())
		return;
	const auto presentStart = SDL_GetTicksNS();
	addStat(STAT_COMPOSITE, presentStart - drawStart);

	if (!_presenter->present())
		return;
	recordPresentTiming();

	if (showStats())
	{
		const auto now = SDL_GetTicksNS();
		addStat(STAT_PRESENT, now - presentStart);
		int late = -1;
		if (s_frameQueuedAt != 0)
		{
			addStat(STAT_TOTAL, now - s_frameQueuedAt);
			late = missedVBlank(s_frameQueuedAt, now);
		}
		commitFrameStats(frameInterval(now), late);
		_statsFrames++;
		/* Shows from the next present on. */
		if (updateStats())
			updatePresenterOverlay();
	}
#endif
}

static std::atomic<WwDisplayClientContext*> s_presentTiming = nullptr;

void SdlWindow::setPresentTimingChannel(WwDisplayClientContext* ww)
{
	s_presentTiming = ww;
}

/* weaselwayd moves the guest's vblank so that frames are ready a few ms
 * before the monitor's. It gets the late tail, the 10th percentile, about
 * twice a second. Without vsync there is no vblank to line up with. */
void SdlWindow::recordPresentTiming()
{
#if defined(_WIN32)
	auto ww = s_presentTiming.load();
	if (!ww || !s_vsync || !vblank())
		return;
	const auto slack = _vblank->slackUs(_presentPendingQpc);
	if (slack < 0)
		return;

	const auto now = SDL_GetTicksNS();
	if (_timingSince == 0)
		_timingSince = now;
	_timingSlackUs.push_back(slack);
	if (now - _timingSince < 500000000ull)
		return;

	/* A few frames after a pause say little. */
	auto& v = _timingSlackUs;
	if (v.size() >= 10)
	{
		const auto pct = [&v](size_t p)
		{
			auto it = v.begin() + static_cast<std::ptrdiff_t>(v.size() * p / 100);
			std::nth_element(v.begin(), it, v.end());
			return *it;
		};
		const auto p10 = pct(10);
		const auto p50 = pct(50);
		const auto p90 = pct(90);
		const auto frames = static_cast<UINT32>(v.size());
		const auto rc = ww->SendPresentTiming(ww, _monitorIndex, frames, p10);
		if (s_statsLog)
			(void)fprintf(s_statsLog, "[timing] %u frames  slack p10/50/90 %d/%d/%d us  rc %u\n",
			              frames, p10, p50, p90, rc);
	}
	v.clear();
	_timingSince = now;
#endif
}

SdlWin32VBlank* SdlWindow::vblank()
{
#if defined(_WIN32)
	if (!_vblank)
	{
		auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(
		    SDL_GetWindowProperties(_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		if (hwnd)
			_vblank = std::make_unique<SdlWin32VBlank>(hwnd);
	}
#endif
	return _vblank.get();
}

/* The swap chain takes a frame at a vblank, so a frame is presented right
 * after the first vblank after it was queued. It is late if it is presented
 * after the second: it shows a refresh later than it could have. */
int SdlWindow::missedVBlank([[maybe_unused]] Uint64 queuedNs, [[maybe_unused]] Uint64 nowNs)
{
#if defined(_WIN32)
	if (!s_vsync || (nowNs < queuedNs) || !vblank())
		return -1;
	LARGE_INTEGER qpc = {};
	LARGE_INTEGER freq = {};
	(void)QueryPerformanceCounter(&qpc);
	(void)QueryPerformanceFrequency(&freq);
	const auto ago = static_cast<double>(SDL_GetTicksNS() - queuedNs) / 1e9 *
	                 static_cast<double>(freq.QuadPart);
	const auto slack = _vblank->slackUs(qpc.QuadPart - static_cast<Sint64>(ago));
	const auto period = _vblank->periodUs();
	if ((slack < 0) || (period <= 0))
		return -1;
	return ((nowNs - queuedNs) / 1000 > static_cast<Uint64>(slack) + static_cast<Uint64>(period))
	           ? 1
	           : 0;
#else
	return -1;
#endif
}

Uint64 SdlWindow::frameInterval(Uint64 nowNs)
{
	const auto interval = (_lastPresentNs != 0) ? nowNs - _lastPresentNs : 0;
	_lastPresentNs = nowNs;
	return interval;
}

bool SdlWindow::takeRedrawRequest()
{
#if defined(_WIN32)
	if (_presenter && _presenter->takeRedrawRequest())
	{
		/* It lost the notification with its device. */
		std::unique_lock lock(_notificationLock);
		_notificationChanged = true;
		return true;
	}
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
	/* Only commitFrameStats() clears it, so a value from before the stats were
	 * turned on would count as the first frame's latency. */
	if (s_showStats)
		s_frameQueuedAt = ns;
}

void SdlWindow::countFrameIn()
{
	if (s_showStats)
		s_framesIn++;
}

void SdlWindow::commitFrameStats(Uint64 intervalNs, int late)
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
	if (s_frameHas[STAT_TOTAL])
		s_latencyMs.push_back(static_cast<float>(s_frame[STAT_TOTAL]) / 1e6f);
	if (late >= 0)
	{
		s_deadlineFrames++;
		if (late > 0)
			s_lateFrames++;
	}

	auto& sample = s_history[s_historyNext];
	for (size_t x = 0; x < sample.ms.size(); x++)
		sample.ms[x] = static_cast<float>(s_frame[x]) / 1e6f;
	sample.intervalMs = static_cast<float>(intervalNs) / 1e6f;
	sample.late = late > 0;
	s_historyNext = (s_historyNext + 1) % s_history.size();
	s_historyCount = std::min(s_historyCount + 1, s_history.size());
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

#if defined(_WIN32)
/* A Material surface: colours 0xRRGGBB, sizes in multiples of the font
 * size. */
struct BoxStyle
{
	Uint32 surface;
	float alpha;
	Uint32 text;
	float radius;
	float padX;
	float padY;
};
/* A Material 3 snackbar (inverse surface, 4dp corners at 14sp). */
static constexpr BoxStyle s_snackbarStyle = { 0x313033, 1.0f, 0xf4eff4, 0.3f, 1.15f, 1.0f };
/* A dark Material 3 card (surface container high, 12dp corners). */
static constexpr BoxStyle s_cardStyle = { 0x2b2930, 0.94f, 0xe6e1e5, 0.9f, 1.2f, 1.0f };

/* Rounded rect's signed distance: negative inside, in pixels. */
static float roundedRectDistance(float px, float py, float x, float y, float w, float h, float r)
{
	const auto qx = std::abs(px - x - w / 2.0f) - (w / 2.0f - r);
	const auto qy = std::abs(py - y - h / 2.0f) - (h / 2.0f - r);
	const auto ox = std::max(qx, 0.0f);
	const auto oy = std::max(qy, 0.0f);
	return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

/* Lines of text on a rounded Material surface with a drop shadow, as
 * premultiplied BGRA32. The box starts marginX/marginY from the top left,
 * or further if the shadow needs the room. Sizes are in points, times scale.
 * GDI draws the text because SDL only has its 8px debug font. colors tints
 * parts of lines (ASCII only). With extraH > 0 the box gets that much empty
 * room below the text, returned in extra, for the caller to draw into. */
static SDL_Surface* renderTextBox(const std::vector<std::string>& lines, const wchar_t* face,
                                  float fontSize, float scale, float marginX, float marginY,
                                  const BoxStyle& style,
                                  const std::vector<SdlTextColor>& colors = {}, int extraH = 0,
                                  SDL_Rect* extra = nullptr)
{
	const auto em = fontSize * scale;
	const auto padX = static_cast<int>(std::lround(em * style.padX));
	const auto padY = static_cast<int>(std::lround(em * style.padY));
	const auto radius = std::round(em * style.radius);
	/* Elevation level 3: a shadow blurred over 8dp, 3dp down. */
	const auto blur = 8.0f * scale;
	const auto drop = 3.0f * scale;
	const auto shadowRoom = static_cast<int>(std::ceil(blur + drop));
	const auto left = std::max(static_cast<int>(std::lround(marginX * scale)), shadowRoom);
	const auto top = std::max(static_cast<int>(std::lround(marginY * scale)), shadowRoom);

	std::vector<std::wstring> wide;
	for (const auto& line : lines)
	{
		const auto len =
		    MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
		auto& w = wide.emplace_back(static_cast<size_t>(std::max(len, 0)), L'\0');
		(void)MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), w.data(),
		                          len);
	}

	auto dc = CreateCompatibleDC(nullptr);
	if (!dc)
		return nullptr;
	/* Grayscale antialiasing: ClearType's colour fringes need an opaque
	 * background to look right. */
	auto font = CreateFontW(-static_cast<int>(std::lround(em)), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
	                        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                        ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
	auto oldFont = SelectObject(dc, font);
	int textW = 0;
	int lineH = 0;
	for (const auto& line : wide)
	{
		SIZE size = {};
		(void)GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &size);
		textW = std::max(textW, static_cast<int>(size.cx));
		lineH = std::max(lineH, static_cast<int>(size.cy));
	}
	const int textH = lineH * static_cast<int>(wide.size());
	const int boxW = textW + 2 * padX;
	const int boxH = textH + 2 * padY + ((extraH > 0) ? extraH + padY : 0);
	const int w = left + boxW + shadowRoom;
	const int h = top + boxH + shadowRoom;
	if (extra)
		*extra = { left + padX, top + 2 * padY + textH, textW, std::max(extraH, 0) };

	struct Tint
	{
		int x0, x1, y0, y1;
		Uint32 rgb;
	};
	std::vector<Tint> tints;
	for (const auto& c : colors)
	{
		if ((c.line >= wide.size()) || (c.begin > c.end) || (c.end > wide[c.line].size()))
			continue;
		SIZE a = {};
		SIZE b = {};
		(void)GetTextExtentPoint32W(dc, wide[c.line].c_str(), static_cast<int>(c.begin), &a);
		(void)GetTextExtentPoint32W(dc, wide[c.line].c_str(), static_cast<int>(c.end), &b);
		const int y = top + padY + lineH * static_cast<int>(c.line);
		tints.push_back({ left + padX + static_cast<int>(a.cx),
		                  left + padX + static_cast<int>(b.cx), y, y + lineH, c.rgb });
	}

	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = w;
	info.bmiHeader.biHeight = -h; /* top down */
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	SDL_Surface* surface = nullptr;
	if (bitmap && bits)
	{
		auto oldBitmap = SelectObject(dc, bitmap);
		/* White on the black DIB: any channel is the text's coverage. */
		(void)SetTextColor(dc, RGB(0xff, 0xff, 0xff));
		(void)SetBkMode(dc, TRANSPARENT);
		for (size_t x = 0; x < wide.size(); x++)
			(void)TextOutW(dc, left + padX, top + padY + lineH * static_cast<int>(x),
			               wide[x].c_str(), static_cast<int>(wide[x].size()));
		(void)GdiFlush();

		surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_BGRA32);
		for (int y = 0; surface && (y < h); y++)
		{
			const auto src = static_cast<const Uint8*>(bits) + 4ll * w * y;
			auto dst = static_cast<Uint8*>(surface->pixels) + 1ll * surface->pitch * y;
			for (int x = 0; x < w; x++)
			{
				/* Text over the box over its shadow, premultiplied. The box
				 * is antialiased over one pixel, the shadow fades out over
				 * the blur. */
				const auto px = static_cast<float>(x) + 0.5f;
				const auto py = static_cast<float>(y) + 0.5f;
				const auto fl = static_cast<float>(left);
				const auto ft = static_cast<float>(top);
				const auto fw = static_cast<float>(boxW);
				const auto fh = static_cast<float>(boxH);
				const auto box =
				    std::clamp(0.5f - roundedRectDistance(px, py, fl, ft, fw, fh, radius), 0.0f,
				               1.0f) *
				    style.alpha;
				const auto s = std::clamp(
				    0.5f - roundedRectDistance(px, py - drop, fl, ft, fw, fh, radius) / blur, 0.0f,
				    1.0f);
				const auto shadow = 0.38f * s * s * (3.0f - 2.0f * s);
				const auto ink = static_cast<float>(src[4 * x + 1]) / 255.0f;

				Uint32 rgb = style.text;
				for (const auto& t : tints)
					if ((x >= t.x0) && (x < t.x1) && (y >= t.y0) && (y < t.y1))
						rgb = t.rgb;
				const auto under = box + shadow * (1.0f - box);
				for (int c = 0; c < 3; c++)
				{
					const auto channel = [](Uint32 v, int c)
					{ return static_cast<float>((v >> (8 * c)) & 0xff); };
					dst[4 * x + c] = static_cast<Uint8>(std::lround(
					    ink * channel(rgb, c) + (1.0f - ink) * box * channel(style.surface, c)));
				}
				dst[4 * x + 3] =
				    static_cast<Uint8>(std::lround((ink + (1.0f - ink) * under) * 255.0f));
			}
		}
		(void)SelectObject(dc, oldBitmap);
	}

	(void)SelectObject(dc, oldFont);
	if (bitmap)
		(void)DeleteObject(bitmap);
	if (font)
		(void)DeleteObject(font);
	(void)DeleteDC(dc);
	return surface;
}

/* One bar per frame in g, newest on the right, ms from 0 to rangeMs; longer
 * times stop at the top. value gives a bar's segments bottom up as
 * (ms, colour, alpha) through add. A line at each refresh period. */
template <typename Value>
static void drawGraph(SDL_Surface* surface, const SDL_Rect& g, int barW, float rangeMs,
                      float periodMs, Value value)
{
	const auto put = [surface](int x, int y, Uint32 rgb, float a)
	{
		auto p = static_cast<Uint8*>(surface->pixels) + 1ll * surface->pitch * y + 4ll * x;
		for (int c = 0; c < 3; c++)
			p[c] = static_cast<Uint8>(std::lround(static_cast<float>((rgb >> (8 * c)) & 0xff) * a +
			                                      static_cast<float>(p[c]) * (1.0f - a)));
		p[3] = static_cast<Uint8>(std::lround(255.0f * a + static_cast<float>(p[3]) * (1.0f - a)));
	};
	/* Pixel rows from the bottom for a time, at most the graph's height. */
	const auto rows = [&g, rangeMs](float ms)
	{
		return std::clamp(static_cast<int>(std::lround(ms / rangeMs * static_cast<float>(g.h))),
		                  0, g.h);
	};

	const auto fit = static_cast<size_t>(g.w / barW);
	const auto count = std::min(s_historyCount, fit);
	for (size_t i = 0; i < count; i++)
	{
		const auto& frame =
		    s_history[(s_historyNext + s_history.size() - count + i) % s_history.size()];
		const int x0 = g.x + g.w - static_cast<int>(count - i) * barW;
		/* A gap between bars once they are wide enough. */
		const int x1 = x0 + ((barW > 2) ? barW - 1 : barW);
		float sum = 0.0f;
		const auto add = [&](float ms, Uint32 rgb, float a)
		{
			const int from = rows(sum);
			sum += ms;
			for (int r = from; r < rows(sum); r++)
				for (int x = x0; x < x1; x++)
					put(x, g.y + g.h - 1 - r, rgb, a);
		};
		value(frame, add);
	}
	for (int k = 1; static_cast<float>(k) * periodMs < rangeMs * 0.99f; k++)
	{
		const int r = std::max(rows(static_cast<float>(k) * periodMs), 1);
		for (int x = g.x; x < g.x + g.w; x++)
			put(x, g.y + g.h - r, 0xffffff, 0.35f);
	}
}

/* Two graphs, one bar per frame: the stages stacked in their colours, in red
 * for a late frame, and below the time since the frame before. Scaled to the
 * monitor's refresh period, so a steady frame rate looks the same at any
 * rate. A gap longer than the graph is idle and only a faint stub. */
static void drawStatsGraphs(SDL_Surface* surface, const SDL_Rect& area, int barW, int gap,
                            float periodMs)
{
	const int stagesH = (area.h - gap) * 2 / 3;
	drawGraph(surface, { area.x, area.y, area.w, stagesH }, barW, 2.0f * periodMs, periodMs,
	          [](const FrameSample& frame, const auto& add)
	          {
		          for (size_t st = 0; st < SdlWindow::STAT_TOTAL; st++)
			          add(frame.ms[st], frame.late ? s_lateColor : s_stageColors[st], 1.0f);
	          });
	const auto rangeMs = 3.0f * periodMs;
	drawGraph(surface, { area.x, area.y + stagesH + gap, area.w, area.h - stagesH - gap }, barW,
	          rangeMs, periodMs,
	          [rangeMs](const FrameSample& frame, const auto& add)
	          {
		          const auto rgb = frame.late ? s_lateColor : 0x938f99;
		          if (frame.intervalMs > rangeMs)
			          add(rangeMs / 8.0f, rgb, 0.3f);
		          else
			          add(frame.intervalMs, rgb, 1.0f);
	          });
}
#endif

/* shortcut: only the Windows presenter draws notifications; the SDL renderer
 * path (RemoteApp, other platforms) would need its own drawing and a way to
 * present without a new frame. */
bool SdlWindow::setNotification([[maybe_unused]] const std::string& text,
                                [[maybe_unused]] float fontSize)
{
#if defined(_WIN32)
	if (!_presenter || (text == _notificationText))
		return false;
	_notificationText = text;
	SDL_Surface* surface = nullptr;
	if (!text.empty())
		surface = renderTextBox({ text }, L"Segoe UI", fontSize,
		                        SDL_GetWindowDisplayScale(_window), 0.0f, 16.0f, s_snackbarStyle);

	std::unique_lock lock(_notificationLock);
	_notification.reset(surface);
	_notificationChanged = true;
	return true;
#else
	return false;
#endif
}

void SdlWindow::updatePresenterOverlay()
{
#if defined(_WIN32)
	if (_statsText.empty())
		return;

	/* The presenter has no text drawing of its own, so the overlay is drawn
	 * into a surface and handed over as a texture. Only when the numbers
	 * change, i.e. twice a second. The stats scale is relative to its default
	 * of 2. */
	const auto fontSize = 6.5f * statsScale();
	const auto scale = SDL_GetWindowDisplayScale(_window);
	const auto em = fontSize * scale;
	auto lines = _statsText;
	auto colors = _statsColors;
	SDL_Rect graphs = {};
	int graphsH = 0;
	/* 60 Hz until the vblank thread has measured the monitor. */
	const auto periodUs = vblank() ? _vblank->periodUs() : -1;
	const auto periodMs = (periodUs > 0) ? static_cast<float>(periodUs) / 1000.0f : 1000.0f / 60.0f;
	if (_statsText.size() > 1)
	{
		char legend[200] = { 0 };
		(void)SDL_snprintf(legend, sizeof(legend),
		                   "graphs per frame: stage times (0-%.0f ms), time since the frame before "
		                   "(0-%.0f ms, longer is idle); lines at %.0f Hz, late in red",
		                   2.0 * periodMs, 3.0 * periodMs, 1000.0 / periodMs);
		lines.emplace_back(legend);
		colors.push_back(
		    { lines.size() - 1, lines.back().size() - 3, lines.back().size(), s_lateColor });
		graphsH = static_cast<int>(std::lround(em * 12.0f));
	}
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
	    renderTextBox(lines, L"Consolas", fontSize, scale, 8.0f, 8.0f, s_cardStyle, colors, graphsH,
	                  &graphs),
	    SDL_DestroySurface);
	if (!surface)
		return;
	if (graphsH > 0)
		drawStatsGraphs(surface.get(), graphs,
		                std::max(1, static_cast<int>(std::lround(em / 4.0f))),
		                static_cast<int>(std::lround(em / 2.0f)), periodMs);
	(void)_presenter->setOverlay(surface.get());
#endif
}

void SdlWindow::syncStatsToggle()
{
	const bool on = showStats();
	if (on == _statsOn)
		return;
	_statsOn = on;

	/* Counted before the toggle, or not at all while off. */
	_statsWindowStart = 0;
	_statsFrames = 0;
	_statsBlits = 0;
	_statsBytes = 0;
	_statsUploads.fill({});
	s_stages.fill({});
	s_frame.fill(0);
	s_frameHas.fill(false);
	s_frameQueuedAt = 0;
	s_framesIn = 0;
	s_latencyMs.clear();
	s_lateFrames = 0;
	s_deadlineFrames = 0;
	s_historyNext = 0;
	s_historyCount = 0;
	_lastPresentNs = 0;
	/* Something to show until the first numbers, half a second of frames
	 * later; an idle desktop sends none. */
	_statsText.clear();
	_statsColors.clear();
	if (on)
		_statsText.emplace_back("stats: waiting for frames");
#if defined(_WIN32)
	if (_presenter && on)
		updatePresenterOverlay();
	else if (_presenter)
		(void)_presenter->setOverlay(nullptr);
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
		const auto bpf = static_cast<double>(_statsBlits) /
		                 static_cast<double>(std::max<Uint64>(_statsFrames, 1));

		const auto in = static_cast<double>(s_framesIn.exchange(0)) / secs;
		int vsync = 0;
		if (_renderer)
			(void)SDL_GetRenderVSync(_renderer, &vsync);
		const char* name = _presenter ? "presenter" : SDL_GetRendererName(_renderer);
		if (_presenter)
			vsync = s_vsync ? 1 : 0;

		char buffer[160] = { 0 };
		_statsText.clear();
		_statsColors.clear();
		(void)SDL_snprintf(buffer, sizeof(buffer),
		                   "%5.1f fps (%5.1f in)  %7.2f MiB/s  %5.1f blits/frame  %dx%d  %s%s", fps,
		                   in, mbps, bpf, _gdiTextureW, _gdiTextureH, name ? name : "?",
		                   (vsync != 0) ? " vsync" : "");
		_statsText.emplace_back(buffer);

		/* Latency from the frame's arrival to its present, and how many frames
		 * missed the first vblank after they arrived. */
		std::string latency = "latency ms";
		auto& v = s_latencyMs;
		if (!v.empty())
		{
			const auto pct = [&v](size_t p)
			{
				auto it = v.begin() + static_cast<std::ptrdiff_t>((v.size() - 1) * p / 100);
				std::nth_element(v.begin(), it, v.end());
				return static_cast<double>(*it);
			};
			double sum = 0.0;
			for (const auto ms : v)
				sum += static_cast<double>(ms);
			(void)SDL_snprintf(buffer, sizeof(buffer), "  avg %.2f  p50 %.2f  p95 %.2f  max %.2f",
			                   sum / static_cast<double>(v.size()), pct(50), pct(95), pct(100));
			latency += buffer;
		}
		else
			latency += "  -";
		if (s_deadlineFrames > 0)
		{
			const auto begin = latency.size() + 4;
			(void)SDL_snprintf(buffer, sizeof(buffer), "    late %llu of %llu (%.1f%%)",
			                   static_cast<unsigned long long>(s_lateFrames),
			                   static_cast<unsigned long long>(s_deadlineFrames),
			                   100.0 * static_cast<double>(s_lateFrames) /
			                       static_cast<double>(s_deadlineFrames));
			latency += buffer;
			_statsColors.push_back({ _statsText.size(), begin, begin + 4, s_lateColor });
		}
		else
			latency += "    late: no vblank to check against";
		_statsText.push_back(latency);
		v.clear();
		s_lateFrames = 0;
		s_deadlineFrames = 0;

		/* avg/max in ms per stage, two lines so it fits a 1280 wide window. */
		static const std::array<const char*, STAT_COUNT> names = {
			"wait", "lock", "upload", "render", "ack", "swapwait", "composite", "present", "total"
		};
		std::string line = "ms avg/max";
		for (size_t x = 0; x < s_stages.size(); x++)
		{
			const auto& stage = s_stages[x];
			const auto begin = line.size() + 2;
			_statsColors.push_back(
			    { _statsText.size(), begin, begin + SDL_strlen(names[x]), s_stageColors[x] });
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

		/* Per upload path, to compare them on the same content: run once with
		 * and once with -sdl-zero-copy. */
		static const std::array<const char*, 2> paths = { "cpu copy ", "zero copy" };
		for (size_t x = 0; x < _statsUploads.size(); x++)
		{
			const auto& path = _statsUploads[x];
			if (path.count == 0)
				continue;
			const auto n = static_cast<double>(path.count);
			const auto mib = static_cast<double>(path.bytes) / (1024.0 * 1024.0);
			(void)SDL_snprintf(buffer, sizeof(buffer),
			                   "upload %s: busy %.3f ms/upload  %.3f ms/MiB  %.2f MiB/upload",
			                   paths[x], static_cast<double>(path.busyNS) / n / 1e6,
			                   static_cast<double>(path.busyNS) / 1e6 / mib, mib / n);
			_statsText.emplace_back(buffer);
		}
		_statsUploads.fill({});

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

	if (SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, 0xff))
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
	syncStatsToggle();

	if (_presenter)
	{
		presenterPresent(damage);
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
		commitFrameStats(frameInterval(now), -1);
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
#if defined(SDL_PROP_WINDOW_CREATE_CONSTRAIN_POPUP_BOOLEAN)
	/* The server places RemoteApp popups; a moved one would map clicks to other points. */
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_CONSTRAIN_POPUP_BOOLEAN, false);
#endif
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
	if (sdl::utils::isWaylandDriver() || sdl::utils::isX11Driver())
		return queryDisplay(id, forceAsPrimary);

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
