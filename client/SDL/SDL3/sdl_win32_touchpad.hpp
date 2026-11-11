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

#include <SDL3/SDL.h>

struct tagMSG;

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
		 * Enabled with /sdl-touchpad-gestures.
		 *
		 * With /sdl-touchpad-raw every contact is forwarded, from the first
		 * finger on, and the server's input stack does pointer motion, taps
		 * and scrolling too. Windows would move its cursor and click and
		 * scroll of the same fingers, so while they are down the cursor is
		 * held where the server says its pointer is, and the mouse input
		 * Windows makes of the touchpad is dropped. */
		namespace touchpad
		{
			/* Registers the Raw Input device. Returns false if registration
			 * failed. */
			[[nodiscard]] bool initialize(bool raw);
			void shutdown();

			/* For each message of SDL's Windows message hook. False if the
			 * message is Windows' click or wheel turn of forwarded contacts,
			 * and must be dropped. */
			[[nodiscard]] bool handleMessage(tagMSG* msg);

			/* True if the mouse event is Windows' idea of the touchpad, made
			 * while its contacts are forwarded raw, and must not be sent. */
			[[nodiscard]] bool dropsMouseEvent(const SDL_Event& ev);

			/* Moves the held cursor to @x, @y in the window's coordinates.
			 * False if the cursor is not held, and is the caller's to move. */
			[[nodiscard]] bool moveCursor(SDL_Window* window, float x, float y);

			/* Switches between forwarding every contact and only gestures of
			 * three or more fingers. Returns whether every one is now. */
			bool toggleRaw();

			/* Lifts all contacts of a gesture in progress. Called when the
			 * window loses focus (Alt+Tab, Win key, ...): no further reports
			 * reach us then, and the server would otherwise be left with a
			 * gesture that only the watchdog ends. */
			void cancelGesture();
		} // namespace touchpad
	} // namespace win32
} // namespace sdl
