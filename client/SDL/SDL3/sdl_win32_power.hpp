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

#pragma once

#include <cstdint>

/* Holds a Windows power request for each WWDISPLAY_INHIBIT_* flag, from any
 * thread. The display request is dropped while Windows is locked, so that
 * locking still turns the monitor off. The first call starts watching the
 * lock state. */
void sdl_win32_power_inhibit(uint32_t flags);
