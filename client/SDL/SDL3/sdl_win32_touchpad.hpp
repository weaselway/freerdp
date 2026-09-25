/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Windows raw touchpad passthrough
 *
 * Copyright 2026 The FreeRDP Contributors
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

namespace sdl
{
	namespace win32
	{
		/* Windows Precision Touchpads do not deliver raw per-finger contacts
		 * through the normal digitizer (WM_TOUCH) or pointer (WM_POINTER /
		 * RegisterPointerInputTarget) input paths: this hardware isn't
		 * classified as a digitizer at all (SM_DIGITIZER reads 0), and
		 * RegisterPointerInputTarget(PT_TOUCHPAD) is rejected with
		 * ERROR_ACCESS_DENIED regardless of process elevation or Authenticode
		 * signing.
		 *
		 * So this registers for Raw Input HID reports on the
		 * Digitizer/TouchPad usage (page 0x0D, usage 0x05), which taps the
		 * HID report stream directly, decodes the contacts with the HidP_*
		 * report-descriptor parser, and forwards 3+ finger gestures as
		 * synthetic SDL finger events into the SdlTouch / RDPEI pipeline.
		 * Enabled with /sdl-touchpad-gestures. */
		namespace touchpad
		{
			/* Registers the Raw Input device and installs the global WM_INPUT
			 * interception hook. Returns false if registration failed. */
			[[nodiscard]] bool initialize();
			void shutdown();

			/* Lifts all contacts of a gesture in progress. Called when the
			 * window loses focus (Alt+Tab, Win key, ...): no further reports
			 * reach us then, and the server would otherwise be left with a
			 * gesture that only the watchdog ends. */
			void cancelGesture();
		} // namespace touchpad
	} // namespace win32
} // namespace sdl
