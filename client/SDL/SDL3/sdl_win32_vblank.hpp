/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The vblank of the monitor a window is on, for weaselway
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

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include <windows.h>

struct IDXGIOutput;

/* A thread that waits for every vblank of the monitor @hwnd is on
 * (IDXGIOutput::WaitForVBlank) and follows the window to another monitor.
 * DWM's timing would be the primary monitor's only. */
class SdlWin32VBlank
{
  public:
	explicit SdlWin32VBlank(HWND hwnd);
	SdlWin32VBlank(const SdlWin32VBlank& other) = delete;
	SdlWin32VBlank(SdlWin32VBlank&& other) = delete;
	SdlWin32VBlank& operator=(const SdlWin32VBlank& other) = delete;
	SdlWin32VBlank& operator=(SdlWin32VBlank&& other) = delete;
	~SdlWin32VBlank();

	/* How long after the QueryPerformanceCounter() time @qpc the monitor's
	 * next vblank is, in microseconds. In (0, one refresh]; -1 if unknown. */
	[[nodiscard]] int32_t slackUs(int64_t qpc);

  private:
	void run();
	void vblank(int64_t qpc);

	HWND _hwnd;
	int64_t _freq = 0;
	std::atomic<bool> _stop = false;

	std::mutex _lock;
	int64_t _last = 0;   /* QPC of the last vblank, 0 if unknown */
	int64_t _period = 0; /* in QPC ticks, 0 if unknown */
	unsigned _outliers = 0;

	std::thread _thread; /* last: starts in the constructor */
};
