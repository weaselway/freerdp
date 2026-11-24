/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows power requests for what the weaselway session inhibits
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

#include "sdl_win32_power.hpp"

#include <mutex>
#include <thread>

#include <windows.h>

#include <winpr/library.h>
#include <freerdp/channels/wwdisplay.h>
#include <freerdp/log.h>

#define TAG CLIENT_TAG("sdl.win32.power")

namespace
{
	/* Power requests are not tied to a thread, unlike SetThreadExecutionState,
	 * and powercfg /requests lists them with the reason. */
	struct Power
	{
		std::mutex lock;
		HANDLE request = nullptr;
		uint32_t flags = 0;
		bool locked = false;
		bool systemHeld = false;
		bool displayHeld = false;

		/* Under the lock. */
		void apply()
		{
			if (!request)
				return;
			hold(PowerRequestSystemRequired, systemHeld, (flags & WWDISPLAY_INHIBIT_SYSTEM) != 0);
			hold(PowerRequestDisplayRequired, displayHeld,
			     ((flags & WWDISPLAY_INHIBIT_DISPLAY) != 0) && !locked);
		}

		void hold(POWER_REQUEST_TYPE type, bool& held, bool want)
		{
			if (held == want)
				return;
			const BOOL ok =
			    want ? PowerSetRequest(request, type) : PowerClearRequest(request, type);
			if (!ok)
			{
				WLog_WARN(TAG, "power request %d: %" PRIu32, static_cast<int>(type),
				          static_cast<uint32_t>(GetLastError()));
				return;
			}
			held = want;
		}
	};

	/* Never freed: the lock watcher uses it until the process exits, and
	 * Windows drops the requests then. */
	Power& power()
	{
		static auto* p = new Power;
		return *p;
	}

	LRESULT CALLBACK lockProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if ((msg == WM_WTSSESSION_CHANGE) &&
		    ((wParam == WTS_SESSION_LOCK) || (wParam == WTS_SESSION_UNLOCK)))
		{
			auto& p = power();
			std::scoped_lock guard(p.lock);
			p.locked = (wParam == WTS_SESSION_LOCK);
			WLog_DBG(TAG, "Windows is %s", p.locked ? "locked" : "unlocked");
			p.apply();
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	/* A message-only window on a thread of its own gets the lock and unlock
	 * notifications. It assumes Windows is unlocked when it starts, which it is
	 * when the user starts the viewer. */
	void watchLock()
	{
		using RegisterFn = BOOL(WINAPI*)(HWND, DWORD);
		/* Loaded at runtime: winpr has functions of the same names. */
		HMODULE wts = LoadLibraryExW(L"wtsapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		const auto registerFn =
		    wts ? GetProcAddressAs(wts, "WTSRegisterSessionNotification", RegisterFn) : nullptr;
		const DWORD notifyForThisSession = 0;

		WNDCLASSW wc = {};
		wc.lpfnWndProc = lockProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"FreeRDP.WeaselwayPower";
		if (!registerFn || !RegisterClassW(&wc))
		{
			WLog_WARN(TAG, "cannot watch the lock state; the display stays on while locked");
			return;
		}
		HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
		                            wc.hInstance, nullptr);
		if (!hwnd || !registerFn(hwnd, notifyForThisSession))
		{
			WLog_WARN(TAG, "cannot watch the lock state: %" PRIu32,
			          static_cast<uint32_t>(GetLastError()));
			return;
		}

		MSG msg = {};
		while (GetMessageW(&msg, nullptr, 0, 0) > 0)
			DispatchMessageW(&msg);
	}
} // namespace

void sdl_win32_power_inhibit(uint32_t flags)
{
	static std::once_flag started;
	std::call_once(started,
	               []
	               {
		               REASON_CONTEXT reason = {};
		               reason.Version = POWER_REQUEST_CONTEXT_VERSION;
		               reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
		               reason.Reason.SimpleReasonString =
		                   const_cast<LPWSTR>(L"The Weaselway desktop asks to stay awake");
		               auto& p = power();
		               p.request = PowerCreateRequest(&reason);
		               if (p.request == INVALID_HANDLE_VALUE)
		               {
			               WLog_WARN(TAG, "PowerCreateRequest: %" PRIu32,
			                         static_cast<uint32_t>(GetLastError()));
			               p.request = nullptr;
		               }
		               std::thread(watchLock).detach();
	               });

	auto& p = power();
	std::scoped_lock guard(p.lock);
	p.flags = flags;
	p.apply();
}
