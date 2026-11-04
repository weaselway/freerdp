/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * The refresh rate of the monitor a window is on, for weaselway
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

#include "sdl_win32_refresh_rate.hpp"

#include <algorithm>
#include <cwchar>
#include <vector>

/* Windows 11; older mingw headers lack it. */
#ifndef QDC_VIRTUAL_REFRESH_RATE_AWARE
#define QDC_VIRTUAL_REFRESH_RATE_AWARE 0x00000040
#endif

static uint64_t to_mhz(const DISPLAYCONFIG_RATIONAL& r)
{
	if (r.Denominator == 0)
		return 0;
	return (1000ULL * r.Numerator + r.Denominator / 2) / r.Denominator;
}

static bool query(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                  std::vector<DISPLAYCONFIG_MODE_INFO>& modes, LONG& rc)
{
	for (int attempt = 0; attempt < 3; attempt++)
	{
		UINT32 npaths = 0;
		UINT32 nmodes = 0;
		rc = GetDisplayConfigBufferSizes(flags, &npaths, &nmodes);
		if (rc != ERROR_SUCCESS)
			return false;
		paths.resize(npaths);
		modes.resize(nmodes);
		rc = QueryDisplayConfig(flags, &npaths, paths.data(), &nmodes, modes.data(), nullptr);
		if (rc == ERROR_INSUFFICIENT_BUFFER)
			continue; /* the configuration changed in between */
		if (rc != ERROR_SUCCESS)
			return false;
		paths.resize(npaths);
		modes.resize(nmodes);
		return true;
	}
	return false;
}

uint32_t sdl_win32_refresh_rate_mhz(HWND hwnd)
{
	MONITORINFOEXW info = {};
	info.cbSize = sizeof(info);
	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (!monitor || !GetMonitorInfoW(monitor, &info))
		return 0;

	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	LONG rc = ERROR_SUCCESS;
	/* Without the flag, Windows before 11 rejects the call: retry plain. */
	if (!query(QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_REFRESH_RATE_AWARE, paths, modes, rc) &&
	    (rc != ERROR_INVALID_PARAMETER || !query(QDC_ONLY_ACTIVE_PATHS, paths, modes, rc)))
		return 0;

	for (const auto& path : paths)
	{
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
		    wcscmp(source.viewGdiDeviceName, info.szDevice) != 0)
			continue;

		/* With dynamic refresh rate, the path's rate is the low virtual one
		 * and the target mode's signal is the physical one. VRR runs below
		 * the mode's rate, so that is the highest already. The target mode
		 * is looked up by ID: its index field means something else when
		 * the caller is virtual-mode aware. */
		uint64_t mhz = to_mhz(path.targetInfo.refreshRate);
		for (const auto& mode : modes)
		{
			if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
			    mode.id == path.targetInfo.id &&
			    mode.adapterId.LowPart == path.targetInfo.adapterId.LowPart &&
			    mode.adapterId.HighPart == path.targetInfo.adapterId.HighPart)
				mhz = std::max(mhz, to_mhz(mode.targetMode.targetVideoSignalInfo.vSyncFreq));
		}
		return mhz > UINT32_MAX ? 0 : static_cast<uint32_t>(mhz);
	}
	return 0;
}
