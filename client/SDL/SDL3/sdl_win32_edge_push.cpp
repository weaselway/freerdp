/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Mouse pushes past the edge of the screen, sent as relative motion
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

#include "sdl_win32_edge_push.hpp"

#include <algorithm>
#include <cstdint>

#include <windows.h>

#include <SDL3/SDL.h>

#include <freerdp/log.h>

#include "sdl_context.hpp"

#define TAG CLIENT_TAG("sdl.win32.edge")

namespace
{
	constexpr USHORT kUsagePageGeneric = 0x01;
	constexpr USHORT kUsageMouse = 0x02;

	/* Whether Windows keeps the cursor from going to this point: off every
	 * monitor, or outside the rectangle ClipCursor holds it in. */
	bool blocked(LONG x, LONG y)
	{
		const POINT pt{ x, y };
		RECT clip{};
		if (GetClipCursor(&clip) && !PtInRect(&clip, pt))
			return true;
		return MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) == nullptr;
	}
} // namespace

bool sdl_win32_edge_push_initialize()
{
	/* No target window: Windows sends WM_INPUT to whichever of our windows
	 * has the focus, and the legacy mouse messages stay as they are. */
	RAWINPUTDEVICE rid{ kUsagePageGeneric, kUsageMouse, 0, nullptr };
	if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
	{
		WLog_WARN(TAG, "RegisterRawInputDevices(Mouse) failed, GetLastError=%" PRIu32,
		          static_cast<UINT32>(GetLastError()));
		return false;
	}
	return true;
}

void sdl_win32_edge_push_shutdown()
{
	RAWINPUTDEVICE rid{ kUsagePageGeneric, kUsageMouse, RIDEV_REMOVE, nullptr };
	(void)RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void sdl_win32_edge_push_handle(SdlContext* sdl, const tagMSG* msg)
{
	if (!sdl || !msg || (msg->message != WM_INPUT) || !sdl->isConnected())
		return;

	RAWINPUT raw{};
	UINT size = sizeof(raw);
	if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &raw, &size,
	                    sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
		return;
	if ((raw.header.dwType != RIM_TYPEMOUSE) ||
	    ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0))
		return;
	const LONG dx = raw.data.mouse.lLastX;
	const LONG dy = raw.data.mouse.lLastY;
	if ((dx == 0) && (dy == 0))
		return;

	/* With relative mouse events in use the motion goes out relative anyway;
	 * a RemoteApp window's edges are not the session's. */
	auto cctx = sdl->common();
	const auto settings = cctx->context.settings;
	if (!freerdp_settings_get_bool(settings, FreeRDP_HasRelativeMouseEvent) ||
	    freerdp_settings_get_bool(settings, FreeRDP_RemoteApplicationMode) ||
	    freerdp_client_use_relative_mouse_events(cctx))
		return;

	SDL_Window* window = SDL_GetMouseFocus();
	if (!window)
		return;
	auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(
	    SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	POINT pt{};
	RECT rc{};
	if (!hwnd || !GetCursorPos(&pt) || !GetClientRect(hwnd, &rc))
		return;
	MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
	if (!PtInRect(&rc, pt))
		return;

	/* Only the part of the motion that goes out over an edge of the window
	 * that the cursor sits on and cannot cross. */
	const bool pushX = ((dx < 0) && (pt.x == rc.left) && blocked(pt.x - 1, pt.y)) ||
	                   ((dx > 0) && (pt.x == rc.right - 1) && blocked(pt.x + 1, pt.y));
	const bool pushY = ((dy < 0) && (pt.y == rc.top) && blocked(pt.x, pt.y - 1)) ||
	                   ((dy > 0) && (pt.y == rc.bottom - 1) && blocked(pt.x, pt.y + 1));
	const LONG px = pushX ? dx : 0;
	const LONG py = pushY ? dy : 0;
	if ((px == 0) && (py == 0))
		return;

	(void)freerdp_client_send_button_event(cctx, TRUE, PTR_FLAGS_MOVE,
	                                       std::clamp<LONG>(px, INT16_MIN, INT16_MAX),
	                                       std::clamp<LONG>(py, INT16_MIN, INT16_MAX));
}
