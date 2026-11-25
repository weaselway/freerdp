/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Mouse pushes past the edge of the screen, sent as relative motion
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

struct tagMSG;
class SdlContext;

/* Windows stops the cursor at the edge of the screen, so the absolute
 * positions the viewer sends never press against the barriers a compositor
 * puts there, such as GNOME's hot corner. While the cursor sits on the edge
 * of a session window where Windows holds it, these send what the mouse
 * moves past it as relative motion, read from Raw Input. */
[[nodiscard]] bool sdl_win32_edge_push_initialize();
void sdl_win32_edge_push_shutdown();
/* For each message of SDL's Windows message hook. */
void sdl_win32_edge_push_handle(SdlContext* sdl, const tagMSG* msg);
