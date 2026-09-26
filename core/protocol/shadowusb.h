// shadowusb: USB-over-WSS through Spice usbredir, for gamepad forwarding.
//
// Architecture (RE confirmed from the Mac ShadowUSB.pkg daemon strings):
//
//   1. POST https://<vm-host>/N/devices
//      body = {"protocol":"spice","type":"spice-html5"}
//      Authorization: Bearer <usb_token>
//      -> response {"data":{"id":"<uuid>","spice_url":"wss://...","spice_secret":"<ticket>"}}
//      where N = JWT.instance and usb_token = tokens.usb from proximus-credentials
//
//   2. Connect over WSS to spice_url (port 443, Sec-WebSocket-Version: 13)
//      -> a binary tunnel carrying the Spice protocol
//
//   3. Spice link handshake:
//      - SpiceLinkHeader (magic "REDQ" + version + size)
//      - SpiceLinkMess (channel_type=1 main, capabilities)
//      - server reply SpiceLinkReply
//      - SpiceLinkAuthMechanism + SpiceLinkEncryptedTicket (with spice_secret)
//
//   4. The Spice MainChannel is then open. On top of it, open a sub-channel:
//      - SpiceUsbRedirChannel (channel_type usbredir)
//
//   5. On that SpiceUsbRedirChannel: standard usbredir frames
//      (HELLO + DEVICE_CONNECT + INTERRUPT_PACKET)
//
// Full stack:
//   TCP+TLS (443) -> WebSocket frames -> SpiceLink -> SpiceMainChannel
//                                               -> SpiceUsbRedirChannel -> usbredir
//
// On Switch: Joycon/Pro Controller -> HID gamepad -> usbredir -> VM.
//
// This module is independent of the video streaming stack (ctrl_chan and co.).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers a Spice remote-console for the USB client. Yields spice_url.
 *
 * Endpoint: POST /<instance>/clients/<usb_client_id>/remote-consoles
 *   body = {"protocol":"spice","type":"spice-html5"}
 *   -> response {"data":{"id":"...","spice_url":"wss://..."}}
 *
 * - vm_host : e.g. `ipv4-gpu-X.frsbg01.compute.shadow.tech`
 * - usb_token : tokens.usb from proximus-credentials
 * - instance : JWT.instance
 * - usb_client_id : id of the usb client returned by POST /N/clients type=usb
 * - out_id : buffer for the returned remote-console id
 * - out_spice_url : buffer for the WSS URL (wss://host:443/path)
 * - out_spice_secret : buffer for the Spice ticket (may come from the launcher
 *                      /clients reply) */
bool shadowusb_register_device(const char *vm_host, const char *usb_token,
                                int instance, const char *usb_client_id,
                                char *out_id, size_t out_id_cap,
                                char *out_spice_url, size_t out_url_cap,
                                char *out_spice_secret, size_t out_secret_cap);

/* Multi-client smoke test: tries the 3 client types (launcher/main/usb), each
 * with its own jwt, against the Spice console endpoint. Tells us which
 * combination gets a response carrying spice_url. */
bool shadowusb_probe_remote_consoles(const char *vm_host, int instance,
                                       const char *launcher_id, const char *launcher_jwt,
                                       const char *main_id, const char *main_jwt,
                                       const char *usb_id, const char *usb_jwt);

#ifdef __cplusplus
}
#endif
