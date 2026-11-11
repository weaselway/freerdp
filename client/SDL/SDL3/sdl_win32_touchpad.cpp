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

#include "sdl_win32_touchpad.hpp"

#include <winpr/windows.h>

#include <hidsdi.h>

#include <algorithm>
#include <iterator>
#include <cinttypes>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <freerdp/log.h>

#define TAG CLIENT_TAG("SDL.touchpad")

#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC static_cast<USHORT>(0x01)
#endif
#ifndef HID_USAGE_GENERIC_X
#define HID_USAGE_GENERIC_X static_cast<USHORT>(0x30)
#endif
#ifndef HID_USAGE_GENERIC_Y
#define HID_USAGE_GENERIC_Y static_cast<USHORT>(0x31)
#endif
#ifndef HID_USAGE_PAGE_DIGITIZER
#define HID_USAGE_PAGE_DIGITIZER static_cast<USHORT>(0x0D)
#endif
#ifndef HID_USAGE_DIGITIZER_TOUCH_PAD
#define HID_USAGE_DIGITIZER_TOUCH_PAD static_cast<USHORT>(0x05)
#endif
#ifndef HID_USAGE_PAGE_BUTTON
#define HID_USAGE_PAGE_BUTTON static_cast<USHORT>(0x09)
#endif

/* Not provided by any standard SDK header; values from the USB HID Usage
 * Tables "Digitizers" page (0x0D), also used by Microsoft's Windows
 * Precision Touchpad HID specification. */
namespace digitizer
{
	constexpr USAGE Finger = 0x22;
	constexpr USAGE TipSwitch = 0x42;
	constexpr USAGE ContactIdentifier = 0x51;
	constexpr USAGE ContactCount = 0x54;
} // namespace digitizer

namespace
{
	constexpr SDL_TouchID kSyntheticTouchpadTouchID = 0x4652445054504144ULL; /* "FRDPTPAD" */
	/* The pad's button, sent as a mouse button of its own. */
	constexpr SDL_MouseID kSyntheticTouchpadMouseID = 0x54504144; /* "TPAD" */
	constexpr Uint64 kFingerIdBias = 0x40000000ULL;

	/* Where in a report's Finger sub-collection to find this finger's fields,
	 * and the logical (device-native) coordinate range to normalize against. */
	struct FingerField
	{
		USHORT linkCollection = 0;
		bool hasX = false;
		bool hasY = false;
		bool hasContactId = false;
		bool hasTipSwitch = false;
		LONG xLogicalMin = 0;
		LONG xLogicalMax = 0;
		LONG yLogicalMin = 0;
		LONG yLogicalMax = 0;
	};

	struct DecodedContact
	{
		UINT32 contactId = 0;
		USHORT linkCollection = 0;
		ULONG rawX = 0;
		ULONG rawY = 0;
		float x = 0.0f;
		float y = 0.0f;
		bool tipDown = false;
	};

	struct DeviceInfo
	{
		std::vector<BYTE> preparsedBuffer; /* owns the memory PHIDP_PREPARSED_DATA points into */
		PHIDP_PREPARSED_DATA preparsed = nullptr;
		std::vector<FingerField> fingers;
		/* Digitizer/ContactCount lives on the top-level collection, not in
		 * any Finger sub-collection. It's the mandatory PTP field that says
		 * how many contacts the *whole* frame carries, which is what makes
		 * report chaining decodable exactly rather than by guesswork -- see
		 * decodeReport(). */
		bool hasContactCount = false;
		USHORT contactCountLinkCollection = 0;

		/* Button 1, the pad's own button (a clickpad's surface), on the
		 * top-level collection. */
		bool hasButton = false;
		USHORT buttonLinkCollection = 0;

		/* Partial frame accumulated across a chain of reports, and how many
		 * contacts of the frame the device still owes us. Per device, so a
		 * second touchpad's reports can't splice into this one's frame. See
		 * decodeReport(). */
		std::vector<DecodedContact> framePending;
		size_t frameRemaining = 0;
	};

	/* Keyed by the Raw Input device handle. Only ever touched from the
	 * main/UI thread (the Win32 message pump). */
	std::unordered_map<HANDLE, DeviceInfo> g_devices;

	/* Contacts are forwarded where they are on the pad: a fraction of the
	 * pad's surface becomes that fraction of the session. The position has
	 * no meaning on the screen -- a touchpad is not registered to a display
	 * -- and the server does not take it for one. It feeds the contacts to a
	 * virtual touchpad, and the compositor's own input stack (libinput)
	 * recognizes swipes and pinches from the fingers' real geometry. */

	/* Precision Touchpad firmware commonly can't fit every simultaneously-
	 * down finger's data into a single HID report once 3+ fingers are down,
	 * so it splits them across several consecutive reports (report
	 * chaining) -- an individual report genuinely omitting an otherwise-
	 * still-down finger is normal, not a lift-off. Confirmed via the
	 * mutter-side gesture logging: without tolerating this, a mid-gesture
	 * report that happened to carry fewer than kMinContactsToForward
	 * fingers looked identical to the user actually lifting off, silently
	 * ending and immediately restarting the whole gesture many times a
	 * second during real use -- discarding all progress right when it
	 * mattered, moments before the real release. Tolerate a finger being
	 * missing from this many consecutive reports before concluding it was
	 * actually lifted; at a typical 100+ Hz report rate this is still
	 * comfortably under 100ms. Only applies once a gesture is already
	 * forwarding (see decodeReport) -- the initial 3-finger threshold
	 * crossing still uses the raw per-report count, so starting a gesture
	 * isn't delayed by this.
	 *
	 * Only a fallback now: devices that expose Contact Count (all Precision
	 * Touchpads are required to) get their frames reassembled exactly in
	 * decodeReport(), so chaining is decoded rather than tolerated and this
	 * never comes into play. */
	constexpr uint32_t kContactGraceReports = 8;

	/* kContactGraceReports only helps while reports keep arriving -- but a
	 * touchpad has nothing left to report once every finger is actually
	 * off the surface, so it typically stops sending HID reports for this
	 * device entirely on full release. That means decodeReport() is never
	 * called again to count the grace period down, and a gesture would be
	 * stuck "active" forever. This is a wall-clock watchdog, independent
	 * of whether any more reports ever arrive.
	 *
	 * It must not simply be "checked whenever we happen to already be
	 * processing some other Win32 message": sdl_run()'s main loop
	 * (sdl_freerdp.cpp) blocks in SDL_WaitEventTimeout(nullptr, 1000), which
	 * on an otherwise-idle client (no repaint, no keyboard/mouse activity)
	 * can genuinely go up to a full second between waking up at all -- far
	 * longer than any reasonable staleness threshold. A real Win32 timer is
	 * the correct primitive here: WM_TIMER is specifically designed to
	 * interrupt an idle GetMessage/MsgWaitForMultipleObjects-style wait on
	 * schedule (which is what SDL_WaitEventTimeout uses under the hood), so
	 * it guarantees the watchdog actually runs on schedule regardless of
	 * any other activity, rather than piggybacking on it. */
	constexpr Uint64 kContactStaleTimeoutMs = 150;
	Uint64 g_lastReportTick = 0;
	UINT_PTR g_staleWatchdogTimerId = 0;

	struct ActiveContact
	{
		uint32_t missingStreak = 0;
		/* Last position forwarded for this contact. FINGER_UP must be
		 * reported there: FreeRDP's rdpei_touch_end() (rdpei_main.c) turns
		 * one touch-up into two wire contacts -- an UPDATE at the given
		 * coordinates, immediately followed by UP at the same ones -- so any
		 * other position would make the lifting finger jump first. */
		float lastX = 0.0f;
		float lastY = 0.0f;
	};

	/* Keyed by the raw HID contact identifier, so DOWN/MOTION/UP can be derived
	 * across reports.
	 * Non-empty if and only if a gesture is currently being forwarded. */
	std::unordered_map<UINT32, ActiveContact> g_activeContacts;

	/* The window that had keyboard focus when the gesture started. Every
	 * event of the gesture, including the watchdog's lifts, goes to it:
	 * SdlContext drops finger events for a window ID it doesn't know (0 in
	 * particular), which used to leave the server with a gesture that never
	 * ended. */
	SDL_WindowID g_gestureWindowID = 0;

	/* /sdl-touchpad-raw: forward every contact, not just gestures of three
	 * or more. */
	bool g_raw = false;

	/* While raw contacts are forwarded, the window whose client area the
	 * cursor is held in: Windows moves it with the same fingers, and the
	 * server, whose pointer they move, says where it is to be. Held on for
	 * kHoldTailMs after the last finger lifts, since the server's pointer
	 * still moves for a frame or two after (g_holdUntil, 0 while fingers
	 * are down). nullptr if the cursor is free. */
	HWND g_holdWindow = nullptr;
	Uint64 g_holdUntil = 0;
	constexpr Uint64 kHoldTailMs = 300;

	/* Not provided below WINVER 0x0603. */
	constexpr auto kInputDeviceTouchpad = static_cast<INPUT_MESSAGE_DEVICE_TYPE>(0x10);

	[[nodiscard]] HWND windowHandle(SDL_Window* window)
	{
		return static_cast<HWND>(SDL_GetPointerProperty(
		    SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	}

	void holdCursorAt(POINT p)
	{
		const RECT r{ p.x, p.y, p.x + 1, p.y + 1 };
		(void)ClipCursor(&r);
	}

	/* Holds the cursor where it is, if that is over the window's client area
	 * and no button is down (its release would be dropped). Otherwise the
	 * fingers are Windows' to handle. */
	[[nodiscard]] bool startHold(SDL_Window* window)
	{
		if (g_holdWindow)
			return true;

		const HWND hwnd = windowHandle(window);
		POINT p{};
		RECT client{};
		if (!hwnd || !GetCursorPos(&p) || !GetClientRect(hwnd, &client))
			return false;
		POINT local = p;
		if (!ScreenToClient(hwnd, &local) || !PtInRect(&client, local))
			return false;
		if (SDL_GetMouseState(nullptr, nullptr) != 0)
			return false;

		holdCursorAt(p);
		g_holdWindow = hwnd;
		g_holdUntil = 0;
		return true;
	}

	void endHold()
	{
		if (!g_holdWindow)
			return;
		(void)ClipCursor(nullptr);
		g_holdWindow = nullptr;
		g_holdUntil = 0;
	}

	[[nodiscard]] FingerField* findFinger(std::vector<FingerField>& fingers,
	                                      USHORT linkCollection)
	{
		for (auto& f : fingers)
		{
			if (f.linkCollection == linkCollection)
				return &f;
		}
		return nullptr;
	}

	/* Parses the device's HID report descriptor once, caching where each
	 * finger's X/Y/ContactIdentifier/TipSwitch usages live (which Finger
	 * sub-collection, and X/Y's device-native logical range). Returns false
	 * if this device doesn't look like a Precision Touchpad after all. */
	[[nodiscard]] bool buildDeviceInfo(HANDLE hDevice, DeviceInfo& info)
	{
		UINT size = 0;
		if ((GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &size) != 0) ||
		    (size == 0))
			return false;

		info.preparsedBuffer.resize(size);
		if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, info.preparsedBuffer.data(),
		                           &size) != size)
			return false;

		info.preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(info.preparsedBuffer.data());

		HIDP_CAPS caps{};
		if (HidP_GetCaps(info.preparsed, &caps) != HIDP_STATUS_SUCCESS)
			return false;

		std::vector<USHORT> fingerLinkCollections;
		if (caps.NumberLinkCollectionNodes > 0)
		{
			ULONG numNodes = caps.NumberLinkCollectionNodes;
			std::vector<HIDP_LINK_COLLECTION_NODE> nodes(numNodes);
			if (HidP_GetLinkCollectionNodes(nodes.data(), &numNodes, info.preparsed) ==
			    HIDP_STATUS_SUCCESS)
			{
				for (ULONG i = 0; i < numNodes; i++)
				{
					if ((nodes[i].LinkUsagePage == HID_USAGE_PAGE_DIGITIZER) &&
					    (nodes[i].LinkUsage == digitizer::Finger))
						fingerLinkCollections.push_back(i);
				}
			}
		}

		if (fingerLinkCollections.empty())
		{
			WLog_WARN(TAG, "no Digitizer/Finger collections found in HID report descriptor");
			return false;
		}

		info.fingers.resize(fingerLinkCollections.size());
		for (size_t i = 0; i < fingerLinkCollections.size(); i++)
			info.fingers[i].linkCollection = fingerLinkCollections[i];

		if (caps.NumberInputValueCaps > 0)
		{
			USHORT numValueCaps = caps.NumberInputValueCaps;
			std::vector<HIDP_VALUE_CAPS> valueCaps(numValueCaps);
			if (HidP_GetValueCaps(HidP_Input, valueCaps.data(), &numValueCaps, info.preparsed) ==
			    HIDP_STATUS_SUCCESS)
			{
				for (USHORT i = 0; i < numValueCaps; i++)
				{
					const auto& vc = valueCaps[i];
					if (vc.IsAlias)
						continue; /* duplicate view of another cap's same bits, skip */

					const USAGE usage = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;

					if ((vc.UsagePage == HID_USAGE_PAGE_DIGITIZER) &&
					    (usage == digitizer::ContactCount))
					{
						info.hasContactCount = true;
						info.contactCountLinkCollection = vc.LinkCollection;
						continue; /* top-level, so findFinger() wouldn't match it */
					}

					auto* f = findFinger(info.fingers, vc.LinkCollection);
					if (!f)
						continue;

					WLog_VRB(TAG,
					         "value cap: linkCollection=%u usagePage=0x%02X usage=0x%02X "
					         "reportId=%u logicalMin=%ld logicalMax=%ld",
					         vc.LinkCollection, vc.UsagePage, usage, vc.ReportID,
					         static_cast<long>(vc.LogicalMin), static_cast<long>(vc.LogicalMax));

					if ((vc.UsagePage == HID_USAGE_PAGE_GENERIC) && (usage == HID_USAGE_GENERIC_X))
					{
						f->hasX = true;
						f->xLogicalMin = vc.LogicalMin;
						f->xLogicalMax = vc.LogicalMax;
					}
					else if ((vc.UsagePage == HID_USAGE_PAGE_GENERIC) &&
					         (usage == HID_USAGE_GENERIC_Y))
					{
						f->hasY = true;
						f->yLogicalMin = vc.LogicalMin;
						f->yLogicalMax = vc.LogicalMax;
					}
					else if ((vc.UsagePage == HID_USAGE_PAGE_DIGITIZER) &&
					         (usage == digitizer::ContactIdentifier))
						f->hasContactId = true;
				}
			}
		}

		if (caps.NumberInputButtonCaps > 0)
		{
			USHORT numButtonCaps = caps.NumberInputButtonCaps;
			std::vector<HIDP_BUTTON_CAPS> buttonCaps(numButtonCaps);
			if (HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &numButtonCaps,
			                       info.preparsed) == HIDP_STATUS_SUCCESS)
			{
				for (USHORT i = 0; i < numButtonCaps; i++)
				{
					const auto& bc = buttonCaps[i];
					const USAGE usageMin = bc.IsRange ? bc.Range.UsageMin : bc.NotRange.Usage;
					const USAGE usageMax = bc.IsRange ? bc.Range.UsageMax : bc.NotRange.Usage;

					if ((bc.UsagePage == HID_USAGE_PAGE_BUTTON) && (usageMin <= 1) &&
					    (1 <= usageMax))
					{
						info.hasButton = true;
						info.buttonLinkCollection = bc.LinkCollection;
						continue;
					}

					if ((bc.UsagePage != HID_USAGE_PAGE_DIGITIZER) ||
					    (usageMin > digitizer::TipSwitch) || (digitizer::TipSwitch > usageMax))
						continue;

					if (auto* f = findFinger(info.fingers, bc.LinkCollection))
						f->hasTipSwitch = true;
				}
			}
		}

		unsigned usable = 0;
		for (const auto& f : info.fingers)
		{
			if (f.hasX && f.hasY && f.hasContactId)
				usable++;

			WLog_INFO(TAG,
			          "finger collection: linkCollection=%u hasX=%d hasY=%d hasContactId=%d "
			          "hasTipSwitch=%d xRange=[%ld,%ld] yRange=[%ld,%ld]",
			          f.linkCollection, f.hasX, f.hasY, f.hasContactId, f.hasTipSwitch,
			          static_cast<long>(f.xLogicalMin), static_cast<long>(f.xLogicalMax),
			          static_cast<long>(f.yLogicalMin), static_cast<long>(f.yLogicalMax));
		}

		WLog_INFO(TAG,
		          "parsed touchpad HID descriptor: %u finger collection(s), %u usable, "
		          "hasContactCount=%d hasButton=%d",
		          static_cast<unsigned>(info.fingers.size()), usable, info.hasContactCount,
		          info.hasButton);

		return usable > 0;
	}

	[[nodiscard]] float normalize(LONG value, LONG lo, LONG hi)
	{
		if (hi <= lo)
			return 0.0f;
		const float v = static_cast<float>(value - lo) / static_cast<float>(hi - lo);
		return std::clamp(v, 0.0f, 1.0f);
	}

	void pushFingerEvent(SDL_WindowID windowID, SDL_EventType type, UINT32 contactId, float x,
	                     float y)
	{
		SDL_Event ev{};
		ev.tfinger.type = type;
		ev.tfinger.timestamp = SDL_GetTicksNS();
		ev.tfinger.touchID = kSyntheticTouchpadTouchID;
		ev.tfinger.fingerID =
		    static_cast<SDL_FingerID>(static_cast<Uint64>(contactId) + kFingerIdBias);
		ev.tfinger.x = x;
		ev.tfinger.y = y;
		ev.tfinger.dx = 0.0f;
		ev.tfinger.dy = 0.0f;
		ev.tfinger.pressure = 1.0f; /* not exposed by the mandatory PTP HID fields */
		ev.tfinger.windowID = windowID;
		SDL_PushEvent(&ev);
	}

	/* Whether the pad's button is held, as last sent. */
	bool g_buttonDown = false;

	/* At the cursor, which is where the server's pointer is while it is held:
	 * the server takes a left click while fingers are down for the pad's. */
	void pushButtonEvent(bool down)
	{
		SDL_Window* window = SDL_GetMouseFocus();
		if (!window)
			return;

		SDL_Event ev{};
		ev.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		ev.button.timestamp = SDL_GetTicksNS();
		ev.button.windowID = SDL_GetWindowID(window);
		ev.button.which = kSyntheticTouchpadMouseID;
		ev.button.button = SDL_BUTTON_LEFT;
		ev.button.down = down;
		ev.button.clicks = 1;
		(void)SDL_GetMouseState(&ev.button.x, &ev.button.y);
		SDL_PushEvent(&ev);
	}

	void setButton(bool down)
	{
		if (down == g_buttonDown)
			return;
		g_buttonDown = down;
		WLog_VRB(TAG, "touchpad button %s", down ? "down" : "up");
		pushButtonEvent(down);
	}

	/* Lifts every forwarded contact where it currently is. */
	void endGesture(const char* reason)
	{
		setButton(false);
		for (const auto& entry : g_activeContacts)
		{
			WLog_VRB(TAG, "touchpad contact id=%" PRIu32 " event=up (%s)", entry.first, reason);
			pushFingerEvent(g_gestureWindowID, SDL_EVENT_FINGER_UP, entry.first,
			                entry.second.lastX, entry.second.lastY);
		}
		g_activeContacts.clear();
		g_gestureWindowID = 0;
		if (g_holdWindow)
			g_holdUntil = SDL_GetTicks() + kHoldTailMs;
	}

	/* Below this many simultaneous fingers, the OS's normal touchpad-to-mouse
	 * (single finger) and two-finger-scroll handling already does the right
	 * thing on its own, driven by relative touchpad movement. Forwarding
	 * those as synthetic *absolute* touch contacts too would fight the real
	 * cursor, since touchpad-surface coordinates aren't screen coordinates
	 * (that's the flicker seen when this was unconditional). Only 3+ finger
	 * gestures -- which the OS doesn't otherwise do anything useful with --
	 * get forwarded. */
	constexpr size_t kMinContactsToForward = 3;

	/* While the cursor is held, Windows does nothing with the fingers, so
	 * every one of them is forwarded. */
	[[nodiscard]] size_t minContactsToForward()
	{
		return g_holdWindow ? 1 : kMinContactsToForward;
	}

	void decodeReport(DeviceInfo& info, BYTE* report, DWORD reportLen)
	{
		g_lastReportTick = SDL_GetTicks();

		SDL_Window* focus = SDL_GetKeyboardFocus();
		if (!focus)
		{
			endGesture("focus lost");
			return;
		}
		if (g_activeContacts.empty())
			g_gestureWindowID = SDL_GetWindowID(focus);
		const SDL_WindowID windowID = g_gestureWindowID;

		/* Every finger slot this report could be decoded into, in
		 * descriptor order, whether or not its TipSwitch says it's touching.
		 * Which of them actually carry data for this frame is decided by the
		 * contact-count framing below -- a report's unused trailing slots
		 * hold stale/garbage values, so they must not be read as contacts. */
		std::vector<DecodedContact> slots;

		for (auto& f : info.fingers)
		{
			if (!f.hasX || !f.hasY || !f.hasContactId)
				continue;

			ULONG xValue = 0;
			ULONG yValue = 0;
			ULONG contactIdValue = 0;

			if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, f.linkCollection,
			                       HID_USAGE_GENERIC_X, &xValue, info.preparsed,
			                       reinterpret_cast<PCHAR>(report),
			                       reportLen) != HIDP_STATUS_SUCCESS)
				continue;
			if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, f.linkCollection,
			                       HID_USAGE_GENERIC_Y, &yValue, info.preparsed,
			                       reinterpret_cast<PCHAR>(report),
			                       reportLen) != HIDP_STATUS_SUCCESS)
				continue;
			if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_DIGITIZER, f.linkCollection,
			                       digitizer::ContactIdentifier, &contactIdValue, info.preparsed,
			                       reinterpret_cast<PCHAR>(report),
			                       reportLen) != HIDP_STATUS_SUCCESS)
				continue;

			/* Devices without a per-finger TipSwitch usage: treat any
			 * decodable finger slot in this report as "down". */
			bool tipDown = true;
			if (f.hasTipSwitch)
			{
				USAGE usageList[16] = {};
				ULONG usageListLength = ARRAYSIZE(usageList);
				tipDown = false;
				if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_DIGITIZER, f.linkCollection,
				                   usageList, &usageListLength, info.preparsed,
				                   reinterpret_cast<PCHAR>(report),
				                   reportLen) == HIDP_STATUS_SUCCESS)
				{
					for (ULONG i = 0; i < usageListLength; i++)
					{
						if (usageList[i] == digitizer::TipSwitch)
						{
							tipDown = true;
							break;
						}
					}
				}
			}

			DecodedContact c;
			c.contactId = static_cast<UINT32>(contactIdValue);
			c.linkCollection = f.linkCollection;
			c.rawX = xValue;
			c.rawY = yValue;
			c.x = normalize(static_cast<LONG>(xValue), f.xLogicalMin, f.xLogicalMax);
			c.y = normalize(static_cast<LONG>(yValue), f.yLogicalMin, f.yLogicalMax);
			c.tipDown = tipDown;
			slots.push_back(c);
		}

		/* Reassemble the frame. A Precision Touchpad reports Contact Count
		 * (the number of contacts in the *frame*, not in this report) in the
		 * frame's first report; the continuation reports of a chained frame
		 * carry a count of 0 and simply refill the same finger slots. So the
		 * count tells us both how many of this report's slots are live and
		 * when the frame is finished -- which is the difference between
		 * knowing what the touchpad said and inferring it.
		 *
		 * With that, a completed frame is authoritative: any contact we
		 * think is down but which the frame doesn't list has genuinely been
		 * lifted, right now. That is what removes the ~150ms hitch on
		 * release -- previously a full lift-off produced no further reports
		 * at all, so neither kContactGraceReports nor anything else could
		 * observe it, and the gesture only ended once the stale-gesture
		 * watchdog fired. A release now ends the gesture on the very report
		 * that reports it, including for a quick flick. */
		bool framed = false;
		std::vector<DecodedContact> frame;

		ULONG contactCount = 0;
		if (info.hasContactCount &&
		    (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_DIGITIZER,
		                        info.contactCountLinkCollection, digitizer::ContactCount,
		                        &contactCount, info.preparsed, reinterpret_cast<PCHAR>(report),
		                        reportLen) == HIDP_STATUS_SUCCESS))
		{
			framed = true;

			if (contactCount > 0)
			{
				/* First report of a new frame. */
				info.framePending.clear();
				info.frameRemaining = contactCount;
			}

			const size_t live = std::min<size_t>(info.frameRemaining, slots.size());
			info.framePending.insert(info.framePending.end(), slots.begin(),
			                      std::next(slots.begin(), static_cast<ptrdiff_t>(live)));
			info.frameRemaining -= live;

			if (info.frameRemaining > 0)
				return; /* chained frame, still incomplete -- wait for the rest */

			frame = std::move(info.framePending);
			info.framePending.clear();
		}
		else
		{
			/* No Contact Count usage on this device: fall back to treating
			 * every decodable slot as live, with kContactGraceReports below
			 * absorbing the chaining this can't see. */
			frame = slots;
		}

		std::vector<DecodedContact> down;
		for (const auto& c : frame)
		{
			if (c.tipDown)
				down.push_back(c);
		}

		/* Before the contacts: the button goes up before the finger that
		 * held it does. Down, it follows a finger that is already there. */
		if (info.hasButton && g_holdWindow && !g_activeContacts.empty())
		{
			USAGE usages[16] = {};
			ULONG count = ARRAYSIZE(usages);
			bool pressed = false;
			if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, info.buttonLinkCollection, usages,
			                   &count, info.preparsed, reinterpret_cast<PCHAR>(report),
			                   reportLen) == HIDP_STATUS_SUCCESS)
				pressed = std::find(usages, usages + count, USAGE{ 1 }) != usages + count;
			setButton(pressed);
		}

		const bool wasForwarding = !g_activeContacts.empty();

		if (!wasForwarding)
		{
			/* Not currently forwarding: use the raw per-report count to
			 * decide whether a gesture is starting. Report chaining can
			 * delay this by a report or two if the very first 3+-finger
			 * touchdown happens to be split, but that's imperceptible --
			 * unlike losing an already-in-progress gesture, which is what
			 * the grace period below exists to prevent. */
			if (g_raw && !down.empty())
				(void)startHold(focus);
			if (down.size() < minContactsToForward())
				return;
			g_holdUntil = 0;

			for (const auto& c : down)
			{
				g_activeContacts.emplace(c.contactId, ActiveContact{ 0, c.x, c.y });

				WLog_VRB(TAG,
				         "touchpad contact id=%" PRIu32 " link=%u event=down rawX=%lu rawY=%lu "
				         "x=%.3f y=%.3f",
				         c.contactId, c.linkCollection, static_cast<unsigned long>(c.rawX),
				         static_cast<unsigned long>(c.rawY), static_cast<double>(c.x),
				         static_cast<double>(c.y));

				pushFingerEvent(windowID, SDL_EVENT_FINGER_DOWN, c.contactId, c.x, c.y);
			}
			return;
		}

		/* Already forwarding: fold in whatever this report *does* contain
		 * -- update positions for known contacts (MOTION), register ones
		 * not seen before (DOWN; covers a finger legitimately added
		 * mid-gesture). */
		std::unordered_map<UINT32, bool> seenThisReport;
		for (const auto& c : down)
		{
			seenThisReport[c.contactId] = true;

			auto it = g_activeContacts.find(c.contactId);
			const bool wasActive = it != g_activeContacts.end();
			const SDL_EventType evtype =
			    wasActive ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_FINGER_DOWN;

			if (!wasActive)
				it = g_activeContacts.emplace(c.contactId, ActiveContact{ 0, c.x, c.y }).first;
			else
				it->second.missingStreak = 0;

			const float px = c.x;
			const float py = c.y;

			WLog_VRB(TAG,
			         "touchpad contact id=%" PRIu32 " link=%u event=%u rawX=%lu rawY=%lu "
			         "x=%.3f y=%.3f",
			         c.contactId, c.linkCollection, static_cast<unsigned>(evtype),
			         static_cast<unsigned long>(c.rawX), static_cast<unsigned long>(c.rawY),
			         static_cast<double>(px), static_cast<double>(py));

			it->second.lastX = px;
			it->second.lastY = py;

			pushFingerEvent(windowID, evtype, c.contactId, px, py);
		}

		/* Age out anything this report didn't mention -- but only treat it
		 * as a real lift-off once it's been missing for kContactGraceReports
		 * consecutive reports in a row, not just this one. */
		std::vector<UINT32> trulyLifted;
		for (auto& entry : g_activeContacts)
		{
			if (seenThisReport.count(entry.first) != 0)
				continue;
			/* A complete contact-count-framed report is authoritative about
			 * which fingers are down, so a missing one is a real lift-off --
			 * no grace period, and no waiting for the watchdog. */
			if (framed || (++entry.second.missingStreak > kContactGraceReports))
				trulyLifted.push_back(entry.first);
		}

		for (UINT32 id : trulyLifted)
		{
			const ActiveContact& contact = g_activeContacts.at(id);

			WLog_VRB(TAG, "touchpad contact id=%" PRIu32 " event=up (missing %u reports)", id,
			         kContactGraceReports + 1);
			pushFingerEvent(windowID, SDL_EVENT_FINGER_UP, id, contact.lastX, contact.lastY);
			g_activeContacts.erase(id);
		}

		if (g_activeContacts.size() < minContactsToForward())
		{
			/* Genuinely dropped below threshold: end everything, matching
			 * the "stay silent below 3" design -- whichever fingers are
			 * still (grace-period-)active don't get to keep going alone. */
			endGesture("below threshold");
		}
	}

	void handleRawInput(LPARAM lParam)
	{
		auto hRawInput = reinterpret_cast<HRAWINPUT>(lParam);

		UINT size = 0;
		if (GetRawInputData(hRawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
			return;
		if (size == 0)
			return;

		std::vector<BYTE> buffer(size);
		const UINT copied =
		    GetRawInputData(hRawInput, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));
		if (copied != size)
			return;

		auto* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
		if (raw->header.dwType != RIM_TYPEHID)
			return;

		auto it = g_devices.find(raw->header.hDevice);
		if (it == g_devices.end())
		{
			DeviceInfo info;
			if (!buildDeviceInfo(raw->header.hDevice, info))
				WLog_WARN(TAG, "failed to parse HID descriptor for touchpad device %p",
				          static_cast<void*>(raw->header.hDevice));
			it = g_devices.emplace(raw->header.hDevice, std::move(info)).first;
		}

		DeviceInfo& info = it->second;
		if (!info.preparsed || info.fingers.empty())
			return;

		const DWORD count = raw->data.hid.dwCount;
		const DWORD reportSize = raw->data.hid.dwSizeHid;
		if ((count == 0) || (reportSize == 0))
			return;

		for (DWORD i = 0; i < count; i++)
		{
			BYTE* report = raw->data.hid.bRawData + (static_cast<size_t>(i) * reportSize);
			decodeReport(info, report, reportSize);
		}
	}

	/* See kContactStaleTimeoutMs: force-ends a gesture that's had no
	 * touchpad report at all for too long.
	 *
	 * Now only a safety net: with Contact Count framing (see decodeReport)
	 * a release is recognized from the report that carries it, so this no
	 * longer sits in the path of an ordinary lift-off -- which is what used
	 * to stall the swipe animation for kContactStaleTimeoutMs before it
	 * finished. It still covers a device going silent mid-gesture without
	 * ever reporting the release. */
	void finalizeStaleGesture()
	{
		if (g_activeContacts.empty())
		{
			if (g_holdUntil && (SDL_GetTicks() >= g_holdUntil))
				endHold();
			return;
		}
		if ((SDL_GetTicks() - g_lastReportTick) < kContactStaleTimeoutMs)
			return;

		endGesture("no reports");
	}

	/* Windows makes clicks of taps and the pad's button, and wheel turns of
	 * two fingers, also after they lifted (a tap's click, the scroll's
	 * inertia). The server makes its own of the forwarded contacts. */
	[[nodiscard]] bool isTouchpadClickOrWheel(const MSG* msg)
	{
		if (!g_raw || (msg->message <= WM_MOUSEMOVE) || (msg->message > WM_MOUSEHWHEEL))
			return false;
		if (!SDL_GetKeyboardFocus())
			return false;

		INPUT_MESSAGE_SOURCE source{};
		if (!GetCurrentInputMessageSource(&source) || (source.deviceType != kInputDeviceTouchpad))
			return false;
		WLog_DBG(TAG, "dropped Windows' mouse message 0x%04X made of the touchpad",
		         static_cast<unsigned>(msg->message));
		return true;
	}

	/* Windows makes its touchpad gestures of injected keys, and the arrows
	 * among them carry the numpad's scan code without the extended flag. SDL
	 * goes by the scan code, so with Num Lock on the session typed 8, 4 and 6
	 * on a swipe. With Num Lock on, a real numpad key is VK_NUMPAD* (or, with
	 * Shift, the navigation key Windows makes of it), so a navigation VK on a
	 * numpad scan code means the navigation key. */
	void fixNavigationKey(MSG* msg)
	{
		switch (msg->message)
		{
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
				break;
			default:
				return;
		}

		switch (msg->wParam)
		{
			case VK_UP:
			case VK_DOWN:
			case VK_LEFT:
			case VK_RIGHT:
			case VK_HOME:
			case VK_END:
			case VK_PRIOR:
			case VK_NEXT:
			case VK_INSERT:
			case VK_DELETE:
				break;
			default:
				return;
		}

		/* With Num Lock off, the real numpad sends these VKs on its own scan
		 * codes, and it has to stay the numpad. */
		if ((GetKeyState(VK_NUMLOCK) & 1) == 0)
			return;

		msg->lParam |= static_cast<LPARAM>(KF_EXTENDED) << 16;
	}

	bool SDLCALL sdl_win32_raw_input_hook(void* userdata, MSG* msg)
	{
		(void)userdata;

		finalizeStaleGesture();

		if (msg)
			fixNavigationKey(msg);

		if (msg && isTouchpadClickOrWheel(msg))
			return false;

		if (!msg || (msg->message != WM_INPUT))
			return true;

		handleRawInput(msg->lParam);
		return true; /* never consume: Raw Input is a parallel tap, not exclusive */
	}
} // namespace

namespace sdl
{
	namespace win32
	{
		namespace touchpad
		{
			bool initialize(bool raw)
			{
				g_raw = raw;

				RAWINPUTDEVICE rid{};
				rid.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
				rid.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
				rid.dwFlags = 0;
				rid.hwndTarget = nullptr; /* deliver to whichever of our windows has focus */

				if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
				{
					WLog_WARN(TAG,
					          "RegisterRawInputDevices(Digitizer/TouchPad) failed, "
					          "GetLastError=%" PRIu32,
					          static_cast<UINT32>(GetLastError()));
					return false;
				}

				SDL_SetWindowsMessageHook(sdl_win32_raw_input_hook, nullptr);

				/* See kContactStaleTimeoutMs's comment: a message-only
				 * (hwndTarget=nullptr) timer, so it's independent of any
				 * particular window's lifetime and posts WM_TIMER straight
				 * to this (the main/UI) thread's queue, guaranteed to wake
				 * an idle SDL_WaitEventTimeout on schedule. */
				g_staleWatchdogTimerId =
				    SetTimer(nullptr, 0, static_cast<UINT>(kContactStaleTimeoutMs), nullptr);
				if (!g_staleWatchdogTimerId)
					WLog_WARN(TAG, "SetTimer for the touchpad gesture watchdog failed, "
					               "GetLastError=%" PRIu32,
					          static_cast<UINT32>(GetLastError()));

				WLog_INFO(TAG, "raw touchpad passthrough active (Raw Input + HID parsing)%s",
				          raw ? ", every contact" : "");
				return true;
			}

			bool dropsMouseEvent(const SDL_Event& ev)
			{
				if (!g_holdWindow)
					return false;
				switch (ev.type)
				{
					case SDL_EVENT_MOUSE_MOTION:
					case SDL_EVENT_MOUSE_WHEEL:
						return true;
					case SDL_EVENT_MOUSE_BUTTON_DOWN:
					case SDL_EVENT_MOUSE_BUTTON_UP:
						return ev.button.which != kSyntheticTouchpadMouseID;
					default:
						return false;
				}
			}

			bool moveCursor(SDL_Window* window, float x, float y)
			{
				if (!g_holdWindow || (windowHandle(window) != g_holdWindow))
					return false;

				/* SDL knows how its window coordinates map to the screen;
				 * the warp is confined like any other move, so let go first. */
				(void)ClipCursor(nullptr);
				SDL_WarpMouseInWindow(window, x, y);
				POINT p{};
				if (GetCursorPos(&p))
					holdCursorAt(p);
				return true;
			}

			bool toggleRaw()
			{
				g_raw = !g_raw;
				if (!g_raw)
				{
					endGesture("raw forwarding off");
					endHold();
				}
				return g_raw;
			}

			void cancelGesture()
			{
				endGesture("focus lost");
				endHold();
			}

			void shutdown()
			{
				SDL_SetWindowsMessageHook(nullptr, nullptr);

				if (g_staleWatchdogTimerId)
				{
					KillTimer(nullptr, g_staleWatchdogTimerId);
					g_staleWatchdogTimerId = 0;
				}

				g_devices.clear();
				g_activeContacts.clear();
				g_gestureWindowID = 0;
				g_buttonDown = false;
				endHold();

				RAWINPUTDEVICE rid{};
				rid.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
				rid.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
				rid.dwFlags = RIDEV_REMOVE;
				rid.hwndTarget = nullptr;
				(void)RegisterRawInputDevices(&rid, 1, sizeof(rid));
			}
		} // namespace touchpad
	} // namespace win32
} // namespace sdl
