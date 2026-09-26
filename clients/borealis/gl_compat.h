/* gl_compat.h - does this build have a DESKTOP GL loader?
 *
 * Five files reached for `<GLFW/glfw3.h>` or `<glad/glad.h>` behind
 * `#ifndef __SWITCH__`, which read as "desktop" for exactly as long as there
 * were two console targets. A PS Vita build lands in that branch and stops:
 * Borealis renders through GXM there and drops glad from its own sources
 * (`PLATFORM_PSV OR PLATFORM_PS4` in its CMakeLists), so neither header exists.
 *
 * So the question is asked once, here, and by NAME. Adding a sixth target means
 * one line in this file rather than five conditions to find.
 */
#pragma once

#if defined(SHADOW_HAVE_DESKTOP_GL)
   /* An explicit answer from the build wins. */
#elif defined(__SWITCH__)
   /* The Switch DOES have glad and GLFW - Borealis builds them against EGL -
    * but not the desktop window management around them. */
#  define SHADOW_HAVE_DESKTOP_GL 0
#  define SHADOW_HAVE_GLAD       1
#elif defined(__vita__) || defined(__psp2__) || defined(BOREALIS_USE_GXM) || defined(BOREALIS_USE_DEKO3D)
#  define SHADOW_HAVE_DESKTOP_GL 0
#  define SHADOW_HAVE_GLAD       0
#else
#  define SHADOW_HAVE_DESKTOP_GL 1
#  define SHADOW_HAVE_GLAD       1
#endif

#ifndef SHADOW_HAVE_GLAD
#  define SHADOW_HAVE_GLAD SHADOW_HAVE_DESKTOP_GL
#endif
