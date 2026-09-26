/* devui - the facade of the development menu bar.
 *
 * This is the ONLY header the application includes. It exposes a handful of free
 * functions, and above all it handles the module vanishing from the Switch
 * build:
 *
 *   - CMake compiles no source from clients/borealis/devui/ for the Switch and does not
 *     define SHADOW_DEV_UI;
 *   - in that case the declarations below become empty `inline` stubs, which the
 *     compiler removes.
 *
 * Consequence: the call sites (stream_view.cpp) have NO #if at all. One reads the
 * application's code without following preprocessor branches, and the bar costs
 * nothing on console - where it would make no sense anyway, for lack of a mouse.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "../ui/hud.hpp"

struct NVGcontext;

namespace devui {

/* What the application lends to the bar. It knows neither StreamView nor
 * Borealis: it calls these functions, nothing more. */
struct Host {
    /* Display toggles - the state stays with the application. */
    std::function<bool()> metricsEnabled;
    std::function<void()> toggleMetrics;
    std::function<bool()> stretchEnabled;
    std::function<void()> toggleStretch;

    /* Synthetic arrow marking the position sent to the VM. */
    std::function<bool()> cursorArrow;
    std::function<void()> toggleCursorArrow;

    /* Diagnostic marker for the cursor returned by the VM. */
    std::function<bool()> cursorDebug;
    std::function<void()> toggleCursorDebug;

    /* Actions. */
    std::function<void()> openShadowMenu;
    std::function<void()> toggleKeyboard;

    /* Information re-read on every frame. */
    std::function<std::string()> videoFormat;
    std::function<std::string()> videoRate;

    /* Metrics panel: the bar exposes the same sections as the pause menu
     * rather than maintaining its own notion of "advanced info". `hud` serves to
     * read titles and states; the toggle goes through the application, which
     * persists the choice. */
    ui::Hud                      *hud = nullptr;
    std::function<void(size_t)>   toggleHudSection;
    std::function<void(size_t)>   toggleHudChart;
};

#ifdef SHADOW_DEV_UI

/* To be called by each stream view, passing its address as `owner`. The Host's
 * functions capture the view: reinstalling from each view is what guarantees
 * they never point at a destroyed one. */
void install(const Host &host, const void *owner);

/* To be called when the view is destroyed. No effect if another view has
 * installed itself in the meantime - the bar then belongs to that one. */
void clear(const void *owner);

/* Height of the bar in the caller's units: the video stream must be offset by
 * that much. It follows the scale set below. */
float barHeight();

/* Apparent size of the bar. Same factor as the metrics panel: without it the
 * bar grows with the window and ends up eating the height. */
void setScale(float s);

/* Draws the bar. To be called LAST, so it sits on top. */
void draw(NVGcontext *vg, float x, float y, float width);

/* Returns true when the click is consumed (so must NOT go to the VM). */
bool onMouseButton(float x, float y, bool pressed);

/* Returns true when the cursor is over the bar or over an open menu. */
bool hovers(float x, float y);

/* Cursor position in Borealis LOGICAL pixels.
 * `glfwGetCursorPos` returns WINDOW coordinates, whereas everything `draw()`
 * manipulates is logical: the framebuffer can be denser than the window (HiDPI,
 * fractional scaling) and Borealis draws after an `nvgScale(windowScale)`. Same
 * formula as GLFWInputManager::updateMouseStates - departing from it offsets the
 * cursor. */
bool pointerLogical(float &x, float &y);

#else  /* Switch build: everything evaporates */

inline void  install(const Host &, const void *)       {}
inline void  clear(const void *)                       {}
inline float barHeight()                               { return 0.0f; }
inline void  setScale(float)                           {}
inline void  draw(NVGcontext *, float, float, float)   {}
inline bool  onMouseButton(float, float, bool)         { return false; }
inline bool  hovers(float, float)                      { return false; }
inline bool  pointerLogical(float &, float &)          { return false; }

#endif

}  // namespace devui
