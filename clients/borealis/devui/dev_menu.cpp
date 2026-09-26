/* devui - the menu contents, specific to Shadow.
 *
 * A deliberate separation: menu_bar.{hpp,cpp} is a generic framework that knows
 * only nanovg; this file is the only one that knows what a Shadow gamepad or a
 * frame refresh is. Adding a debug entry is therefore done here, in one line,
 * without touching the drawing or the input.
 */
#ifdef SHADOW_DEV_UI

#include "devui.hpp"
#include "menu_bar.hpp"

#include <borealis.hpp>
#include "../gl_compat.h"
#if SHADOW_HAVE_DESKTOP_GL
#include <GLFW/glfw3.h>
#endif

#include <cstdio>
#include <cstdlib>

extern "C" {
#include "../../../core/protocol/ctrl_gamepad.h"
#include "../../../core/protocol/ctrl_session.h"
}

/* G33: capture of the frame ACTUALLY displayed (glReadPixels), implemented in
 * stream_view.cpp. n=1 photo, n>1 burst. Writes PPMs into /tmp/halyard/. */
extern "C" void stream_view_capture(int n);

/* G46: key injection from the menu (implemented in stream_view.cpp). Linux evdev
 * scancodes, like the rest of the keyboard path. */
extern "C" void stream_view_send_key(int evdev_code);
extern "C" void stream_view_send_combo(int mod1, int key, int mod2);

namespace devui {

namespace {

MenuBar     g_bar;
Host        g_host;
bool        g_installed = false;
const void *g_owner     = nullptr;

std::string ask(const std::function<std::string()> &fn)
{
    return fn ? fn() : std::string("—");
}

void buildMenus()
{
    g_bar.clear();

    /* Labels in English: this bar is a development tool, not a surface meant
     * for the end user (that one is translated). */
    Menu &view = g_bar.add("View");
    view.toggle("Metrics panel",
               [] { return g_host.metricsEnabled && g_host.metricsEnabled(); },
               [] { if (g_host.toggleMetrics) g_host.toggleMetrics(); })
       .toggle("Synthetic cursor arrow",
               [] { return g_host.cursorArrow && g_host.cursorArrow(); },
               [] { if (g_host.toggleCursorArrow) g_host.toggleCursorArrow(); })
       .toggle("Cursor marker reported by the VM",
               [] { return g_host.cursorDebug && g_host.cursorDebug(); },
               [] { if (g_host.toggleCursorDebug) g_host.toggleCursorDebug(); })
       .toggle("Stretch image (ignore aspect ratio)",
               [] { return g_host.stretchEnabled && g_host.stretchEnabled(); },
               [] { if (g_host.toggleStretch) g_host.toggleStretch(); });

    /* The panel's sections are the same here and in the pause menu: one list,
     * one persisted setting. */
    if (g_host.hud && g_host.hud->sectionCount() > 0) {
        view.separator();
        for (size_t i = 0; i < g_host.hud->sectionCount(); i++) {
            view.toggle("Section: " + g_host.hud->sectionTitle(i),
                       [i] { return g_host.hud && g_host.hud->sectionEnabled(i); },
                       [i] { if (g_host.toggleHudSection) g_host.toggleHudSection(i); });
        }
    }

    if (g_host.hud && g_host.hud->chartCount() > 0) {
        view.separator();
        for (size_t i = 0; i < g_host.hud->chartCount(); i++) {
            view.toggle("Graph: " + g_host.hud->chartTitle(i),
                       [i] { return g_host.hud && g_host.hud->chartEnabled(i); },
                       [i] { if (g_host.toggleHudChart) g_host.toggleHudChart(i); });
        }
    }

    view.separator()
       .info("Format", [] { return ask(g_host.videoFormat); })
       .info("Rate",   [] { return ask(g_host.videoRate); });

    g_bar.add("Actions")
        .action("Open Shadow menu",
                [] { if (g_host.openShadowMenu) g_host.openShadowMenu(); })
        .action("On-screen keyboard",
                [] { if (g_host.toggleKeyboard) g_host.toggleKeyboard(); })
        .separator()
        /* What the official client sends when its window is resized (a 0x64 byte
         * on :base+20) - useful when the picture has frozen. */
        .action("Refresh picture", [] { ctrl_session_request_refresh(); });

    /* G46 - keys the Linux desktop intercepts before us (GNOME reserves
     * Super/Win: the window never receives the event, it opens Ubuntu's
     * Activities). So we send them to the VM explicitly. */
    g_bar.add("Keys")
        .action("Send Windows key",     [] { stream_view_send_key(125); })
        /* The signature is (mod1, KEY, mod2): Delete is the key STRUCK, Alt a
         * modifier HELD. The previous order (29, 56, 111) pressed Delete as a
         * modifier and typed Alt - so the VM never saw a real
         * Ctrl+Alt+Delete. */
        .action("Send Ctrl+Alt+Del",    [] { stream_view_send_combo(29, 111, 56); })
        .action("Send Alt+Tab",         [] { stream_view_send_combo(56, 15, 0); })
        .action("Send Win+D (desktop)", [] { stream_view_send_combo(125, 32, 0); });

    /* G33 - capture of what is ON SCREEN, to be triggered exactly when the
     * artefact is visible. Writes PPMs into /tmp/halyard/capture_*.ppm. */
    g_bar.add("Capture")
        .action("Photo (1 image)",        [] { stream_view_capture(1); })
        .action("Burst (30 frames)",     [] { stream_view_capture(30); })
        .action("Rafale longue (120)",    [] { stream_view_capture(120); });

    Menu &pad = g_bar.add("Gamepad");
    /* G45: two distinct states - the PHYSICAL gamepad (evdev probe) and the
     * network channel. We displayed the channel, hence a "connected" while no
     * gamepad was plugged in. */
    pad.info("Controller", [] {
           return std::string(ctrl_gamepad_present() ? "plugged in" : "none detected");
       })
       .info("Channel", [] {
           return std::string(ctrl_gamepad_active() ? "open" : "closed");
       })
       .separator()
       .action("Announce plug-in", [] { ctrl_gamepad_plug(); });

    static const char *BTN[] = { "Square", "Triangle", "Cross", "Circle" };
    for (int b = 0; b < 4; b++) {
        char label[48];
        snprintf(label, sizeof(label), "Send %s", BTN[b]);
        pad.action(label, [b] {
            ctrl_gamepad_button(b, true);
            ctrl_gamepad_button(b, false);
        });
    }

    /* Axis probe: sweeps a single index in a loop so one can see on screen
     * which stick moves. The two vertical indices are the only mappings the
     * decoding did not settle - we measure instead of assuming, elimination
     * having already proved wrong twice. */
    pad.separator()
       .radio("Axis probe: none",
              [] { return ctrl_gamepad_axis_probing() < 0; },
              [] { ctrl_gamepad_axis_sweep(-1); });
    for (int a = 0; a <= 5; a++) {
        char label[48];
        snprintf(label, sizeof(label), "Axis probe: index %d", a);
        pad.radio(label,
                  [a] { return ctrl_gamepad_axis_probing() == a; },
                  [a] { ctrl_gamepad_axis_sweep(a); });
    }
}

}  // namespace

void install(const Host &host, const void *owner)
{
    /* DEMO-1 2026-09-26 - SHADOW_DEV_MENU=0 hides the bar. The demo mode hides
     * it unless asked: its screenshots show the app as a user sees it, and a
     * user has no developer menu. Not installed = height 0 and nothing drawn. */
    const char *want = std::getenv("SHADOW_DEV_MENU");
    const char *demo = std::getenv("SHADOW_DEMO");
    const bool shown = want ? std::atoi(want) != 0 : !(demo && std::atoi(demo) != 0);
    if (!shown) return;
    g_host      = host;
    g_owner     = owner;
    g_installed = true;
    buildMenus();
}

void clear(const void *owner)
{
    if (g_owner != owner) return;
    g_bar.clear();
    g_host      = Host();
    g_owner     = nullptr;
    g_installed = false;
}

float barHeight()
{
    return g_installed ? g_bar.height() : 0.0f;
}

void setScale(float s) { g_bar.setScale(s); }

bool pointerLogical(float &lx, float &ly)
{
    GLFWwindow *win = glfwGetCurrentContext();
    if (!win) return false;
    double cx = 0, cy = 0;
    glfwGetCursorPos(win, &cx, &cy);

    float ws = brls::Application::windowScale;
    if (ws <= 0) ws = 1.0f;
    float sf = 1.0f;
    if (brls::Application::getPlatform() &&
        brls::Application::getPlatform()->getVideoContext())
        sf = (float)brls::Application::getPlatform()->getVideoContext()->getScaleFactor();
    if (sf <= 0) sf = 1.0f;

    lx = (float)(cx * sf / ws);
    ly = (float)(cy * sf / ws);
    return true;
}

void draw(NVGcontext *vg, float x, float y, float width)
{
    if (!g_installed) return;
    /* The hover is refreshed here: the bar then has a single entry point on
     * the application's side, and nobody has to remember to feed it. */
    float px = 0, py = 0;
    if (pointerLogical(px, py)) g_bar.pointerMoved(px, py);
    g_bar.draw(vg, x, y, width);
}

bool onMouseButton(float x, float y, bool pressed)
{
    return g_installed && g_bar.onMouseButton(x, y, pressed);
}

bool hovers(float x, float y)
{
    return g_installed && g_bar.hovers(x, y);
}

}  // namespace devui

#endif /* SHADOW_DEV_UI */
