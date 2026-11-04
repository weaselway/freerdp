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

#pragma once

#include <cstdint>

#include <windows.h>

/* The physical refresh rate in millihertz of the monitor @hwnd is on: the
 * highest it runs at with dynamic refresh rate or VRR. 0 if unknown. */
[[nodiscard]] uint32_t sdl_win32_refresh_rate_mhz(HWND hwnd);
