/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Weaselway display information channel
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

#ifndef FREERDP_CHANNEL_WWDISPLAY_H
#define FREERDP_CHANNEL_WWDISPLAY_H

#include <freerdp/api.h>
#include <freerdp/dvc.h>
#include <freerdp/types.h>

/*
 * What weaselwayd needs to know about each monitor of the client's that RDP
 * has no field for. The server opens the channel; the client sends.
 *
 * Every PDU, little endian: UINT16 type, UINT16 monitor, then the body. The
 * monitor is the index into the client's monitor layout, which is also the
 * gfxredir windowId the server draws that monitor into.
 *
 * WWDISPLAY_PDU_REFRESH_RATE: UINT32 refresh rate in millihertz. The
 * physical rate of the monitor, the highest it runs at with variable or
 * dynamic refresh rate. Sent when the channel opens and when it changes.
 *
 * WWDISPLAY_PDU_PRESENT_TIMING: UINT32 frames, INT32 slack in microseconds.
 * How long before the monitor's next vblank the frames were ready to be
 * shown, the 10th percentile over the last @frames frames; negative if they
 * were ready after it. Sent about twice a second while frames arrive and the
 * client waits for vblank, so the server can move its frames in time.
 */
#define WWDISPLAY_CHANNEL_NAME "wwdisplay"
#define WWDISPLAY_DVC_CHANNEL_NAME "Weaselway::Display"

#define WWDISPLAY_PDU_REFRESH_RATE 1
#define WWDISPLAY_PDU_PRESENT_TIMING 2

#define WWDISPLAY_MAX_MONITORS 16

#endif /* FREERDP_CHANNEL_WWDISPLAY_H */
