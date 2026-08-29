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
#include <limits>
#include <sstream>
#include <cmath>
#include <utility>

#include "sdl_window.hpp"
#include "sdl_utils.hpp"

#include <freerdp/utils/string.h>

SdlWindow::SdlWindow(SDL_DisplayID id, const std::string& title, const SDL_Rect& rect,
                     [[maybe_unused]] Uint32 flags)
    : _initialW(rect.w), _initialH(rect.h), _displayID(id)
{
	auto props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, rect.x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, rect.y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, rect.w);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, rect.h);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);

	if (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);

	if (flags & SDL_WINDOW_FULLSCREEN)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);

	if (flags & SDL_WINDOW_BORDERLESS)
		SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);

	_window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);
	SDL_SetHint(SDL_HINT_APP_NAME, "");
	std::ignore = SDL_SyncWindow(_window);

	_renderer = SDL_CreateRenderer(_window, nullptr);

	/* SDL3 creates renderers with vsync disabled, so presents tear. Enabling it
	 * makes SDL_RenderPresent block until the next refresh, which throttles the
	 * update path in updateSurface() to the display rate. */
	if (_renderer)
	{
		if (!SDL_SetRenderVSync(_renderer, 1))
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "SDL_SetRenderVSync: %s", SDL_GetError());
	}

	std::ignore = resizeToScale();

	_monitor = query(_window, id, true);
}

SdlWindow::SdlWindow(SdlWindow&& other) noexcept
    : _window(other._window), _renderer(other._renderer), _renderTarget(other._renderTarget),
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
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateTexture (render target): %s",
		             SDL_GetError());
}

bool SdlWindow::drawRect(SDL_Surface* surface, SDL_Point offset, const SDL_Rect& srcRect)
{
	WINPR_ASSERT(surface);
	SDL_Rect dstRect = { offset.x + srcRect.x, offset.y + srcRect.y, srcRect.w, srcRect.h };
	return blit(surface, srcRect, dstRect);
}

bool SdlWindow::drawRects(SDL_Surface* surface, SDL_Point offset,
                          const std::vector<SDL_Rect>& rects)
{
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
	SDL_Rect dstRect = srcRect;
	dstRect.x = static_cast<Sint32>(static_cast<float>(dstRect.x) * scale.x);
	dstRect.w = static_cast<Sint32>(static_cast<float>(dstRect.w) * scale.x);
	dstRect.y = static_cast<Sint32>(static_cast<float>(dstRect.y) * scale.y);
	dstRect.h = static_cast<Sint32>(static_cast<float>(dstRect.h) * scale.y);
	return blit(surface, srcRect, dstRect);
}

bool SdlWindow::drawScaledRects(SDL_Surface* surface, const SDL_FPoint& scale,
                                const std::vector<SDL_Rect>& rects)
{
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

bool SdlWindow::blit(SDL_Surface* surface, const SDL_Rect& srcRect, SDL_Rect& dstRect)
{
	if (!_renderer || !surface)
		return false;

	/* Before rendering into it, not after: updateSurface() used to be the only
	 * caller, so a missing or stale-sized target meant this blit went to the
	 * window backbuffer instead and was then overwritten by the target's older
	 * contents -- one frame behind, every frame. */
	ensureRenderTarget();

	/* Lazily create or recreate the persistent GDI texture */
	if (!_gdiTexture || _gdiTextureW != surface->w || _gdiTextureH != surface->h)
	{
		if (_gdiTexture)
			SDL_DestroyTexture(_gdiTexture);
		_gdiTexture = SDL_CreateTexture(_renderer, surface->format, SDL_TEXTUREACCESS_STREAMING,
		                                surface->w, surface->h);
		if (!_gdiTexture)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_CreateTexture: %s", SDL_GetError());
			return false;
		}
		_gdiTextureW = surface->w;
		_gdiTextureH = surface->h;
	}

	/* Upload only the dirty region */
	const auto* details = SDL_GetPixelFormatDetails(surface->format);
	const int bpp = details ? details->bytes_per_pixel : 4;
	const auto* pixels = static_cast<const uint8_t*>(surface->pixels) +
	                     (1ll * srcRect.y * surface->pitch) + (1ll * srcRect.x * bpp);
	if (!SDL_UpdateTexture(_gdiTexture, &srcRect, pixels, surface->pitch))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "SDL_UpdateTexture: %s", SDL_GetError());
		return false;
	}

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

	if (showDamage())
		_damageRects.push_back(dstRect);

	return true;
}

/* Set from the command line before the first window is created, read from the
 * SDL thread only. */
static bool s_showDamage = false;
static bool s_showStats = false;
static float s_statsScale = 2.0f;

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
	const auto now = SDL_GetTicksNS();
	if (_statsWindowStart == 0)
		_statsWindowStart = now;

	/* Recompute at most twice a second: at frame rate the numbers flicker too
	 * fast to read, and the averaging is what makes them meaningful. */
	const auto elapsed = now - _statsWindowStart;
	if (elapsed >= 500000000ull)
	{
		const auto secs = static_cast<double>(elapsed) / 1000000000.0;
		const auto fps = static_cast<double>(_statsFrames) / secs;
		const auto mbps = static_cast<double>(_statsBytes) / secs / (1024.0 * 1024.0);
		const auto bpf = (_statsFrames > 0) ? (_statsBlits / _statsFrames) : _statsBlits;

		char buffer[128] = { 0 };
		(void)SDL_snprintf(buffer, sizeof(buffer),
		                   "%5.1f fps  %7.2f MiB/s  %llu blits/frame  %dx%d", fps, mbps,
		                   static_cast<unsigned long long>(bpf), _gdiTextureW, _gdiTextureH);
		_statsText = buffer;

		_statsWindowStart = now;
		_statsFrames = 0;
		_statsBlits = 0;
		_statsBytes = 0;
	}

	if (_statsText.empty())
		return;

	const auto scale = statsScale();
	float sx = 1.0f;
	float sy = 1.0f;
	(void)SDL_GetRenderScale(_renderer, &sx, &sy);
	if (!SDL_SetRenderScale(_renderer, scale, scale))
		return;

	/* Backing box, so the text stays legible over arbitrary desktop content.
	 * Coordinates are in scaled units from here on. */
	const auto chars = static_cast<float>(_statsText.size());
	const SDL_FRect box{ 2.0f, 2.0f,
		                 chars * static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) + 8.0f,
		                 static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) + 8.0f };
	SDL_BlendMode blend = SDL_BLENDMODE_NONE;
	(void)SDL_GetRenderDrawBlendMode(_renderer, &blend);

	if (SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND) &&
	    SDL_SetRenderDrawColor(_renderer, 0x00, 0x00, 0x00, 0xc0))
		(void)SDL_RenderFillRect(_renderer, &box);

	if (SDL_SetRenderDrawColor(_renderer, 0x00, 0xff, 0x00, 0xff))
		(void)SDL_RenderDebugText(_renderer, box.x + 4.0f, box.y + 4.0f, _statsText.c_str());

	(void)SDL_SetRenderDrawBlendMode(_renderer, blend);
	(void)SDL_SetRenderScale(_renderer, sx, sy);
}

void SdlWindow::updateSurface()
{
	/* Taken by move so the rects are dropped even if a step below bails out;
	 * otherwise a failed present would leave them to be drawn again next
	 * frame, which is exactly what this is meant to show is not happening. */
	const auto damage = std::move(_damageRects);
	_damageRects.clear();

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

	if (!SDL_RenderPresent(_renderer))
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "updateSurface: SDL_RenderPresent: %s",
		             SDL_GetError());
		return;
	}
}

SdlWindow SdlWindow::create(SDL_DisplayID id, const std::string& title, Uint32 flags, Uint32 width,
                            Uint32 height)
{
	flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

	SDL_Rect rect = { static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(id)),
		              static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(id)), static_cast<int>(width),
		              static_cast<int>(height) };

	if ((flags & SDL_WINDOW_FULLSCREEN) != 0)
	{
		std::ignore = SDL_GetDisplayBounds(id, &rect);
	}

	SdlWindow window{ id, title, rect, flags };

	if ((flags & SDL_WINDOW_FULLSCREEN) != 0)
	{
		window.setOffsetX(rect.x);
		window.setOffsetY(rect.y);
	}

	return window;
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
