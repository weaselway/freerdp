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

#include "sdl_win32_vblank.hpp"

#include <dxgi.h>

/* The output that shows @monitor, on whichever adapter drives it; it need
 * not be the one the presenter renders on. */
static IDXGIOutput* find_output(HMONITOR monitor)
{
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return nullptr;

	IDXGIOutput* found = nullptr;
	IDXGIAdapter1* adapter = nullptr;
	for (UINT a = 0; !found && SUCCEEDED(factory->EnumAdapters1(a, &adapter)); a++)
	{
		IDXGIOutput* output = nullptr;
		for (UINT o = 0; !found && SUCCEEDED(adapter->EnumOutputs(o, &output)); o++)
		{
			DXGI_OUTPUT_DESC desc = {};
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
				found = output;
			else
				output->Release();
		}
		adapter->Release();
	}
	factory->Release();
	return found;
}

SdlWin32VBlank::SdlWin32VBlank(HWND hwnd) : _hwnd(hwnd)
{
	LARGE_INTEGER freq = {};
	(void)QueryPerformanceFrequency(&freq);
	_freq = freq.QuadPart;
	_thread = std::thread([this] { run(); });
}

SdlWin32VBlank::~SdlWin32VBlank()
{
	/* WaitForVBlank() returns within a refresh. */
	_stop = true;
	_thread.join();
}

void SdlWin32VBlank::run()
{
	/* The wake-up is the measurement: the less it is delayed the better. */
	(void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	HMONITOR monitor = nullptr;
	IDXGIOutput* output = nullptr;
	while (!_stop)
	{
		const auto now = MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST);
		if (now != monitor)
		{
			if (output)
				output->Release();
			output = find_output(now);
			monitor = now;
			std::scoped_lock lock(_lock);
			_last = 0;
			_period = 0;
		}

		if (!output || FAILED(output->WaitForVBlank()))
		{
			/* Monitor off or gone: look again later. */
			Sleep(100);
			monitor = nullptr;
			continue;
		}
		LARGE_INTEGER qpc = {};
		(void)QueryPerformanceCounter(&qpc);
		vblank(qpc.QuadPart);
	}
	if (output)
		output->Release();
}

/* The period is smoothed over the intervals between wake-ups. One that is
 * far off (a vblank slept through) is skipped, unless they all are: then the
 * rate changed. */
void SdlWin32VBlank::vblank(int64_t qpc)
{
	std::scoped_lock lock(_lock);
	const auto delta = (_last != 0) ? qpc - _last : 0;
	_last = qpc;
	if (delta <= 0)
		return;

	if (_period == 0 || _outliers >= 8)
	{
		_period = delta;
		_outliers = 0;
	}
	else if (delta > _period * 3 / 4 && delta < _period * 5 / 4)
	{
		_period += (delta - _period) / 16;
		_outliers = 0;
	}
	else
		_outliers++;
}

int32_t SdlWin32VBlank::periodUs()
{
	std::scoped_lock lock(_lock);
	if (_period == 0 || _freq == 0)
		return -1;
	return static_cast<int32_t>(_period * 1000000 / _freq);
}

int32_t SdlWin32VBlank::slackUs(int64_t qpc)
{
	std::scoped_lock lock(_lock);
	if (_period == 0 || _last == 0 || _freq == 0)
		return -1;

	auto phase = (qpc - _last) % _period;
	if (phase < 0)
		phase += _period;
	return static_cast<int32_t>((_period - phase) * 1000000 / _freq);
}
