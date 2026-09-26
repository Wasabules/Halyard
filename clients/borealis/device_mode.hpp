/* device_mode - the two questions the console can answer, and on which the
 * stream's quality depends:
 *
 *   - are we docked or handheld? The built-in screen is 1280x720; asking for
 *     1080p in handheld mode encodes and then transmits twice as many pixels as
 *     anyone will ever see;
 *   - is the link Wi-Fi or Ethernet? The console's Wi-Fi tops out well below the
 *     dock's adapter, and a single bitrate setting forces you either to saturate
 *     one or to throttle the other.
 *
 * Off Switch, both answer the value that changes nothing: "not docked" and
 * "unknown link". Callers must therefore treat `LinkType::Unknown` as "change
 * nothing", never as a default.
 */
#pragma once

namespace device {

enum class LinkType { Unknown, WiFi, Ethernet };

/* true when the console sits on its dock (TV mode). false in handheld mode AND
 * on every other platform. */
bool isDocked();

/* The active link type. Queries the console's network service; returns
 * `Unknown` if the service answers badly or does not exist. */
LinkType linkType();

/* Short label for display: "Wi-Fi", "Ethernet", or "—". */
const char *linkLabel();

/* AF10 2026-09-10 - gives back the HOS services this module opened (nifm).
 * Called once, from main's exit path, next to sslExit(). A no-op off Switch. */
void releaseServices();

}  // namespace device
