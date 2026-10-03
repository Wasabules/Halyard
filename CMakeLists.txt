cmake_minimum_required(VERSION 3.10)

# Borealis vendors tweeny, whose CMakeLists asks for CMake 3.0. CMake 4 removed
# compatibility below 3.5 and refuses to configure it: MSYS2 ships CMake 4, and
# the first public Windows run died there. Older CMake ignores the variable. A
# cache default, so a `-D` on the command line still wins.
if (NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Oldest policy version a vendored CMakeLists may ask for")
endif ()

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/borealis/library)

# build options
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

# Dependencies
option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)

if (APPLE AND PLATFORM_DESKTOP)
    option(BUNDLE_MACOS_APP "Bundle a app for macOS" OFF)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
    set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET[arch=arm64] "11.0" CACHE STRING "" FORCE)
    message(STATUS "CMAKE_OSX_ARCHITECTURES: ${CMAKE_OSX_ARCHITECTURES}")
endif ()

# toolchain
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# === NO BUILD MACHINE PATHS IN THE SHIPPED BINARY =======================
#
# `assert()`, `__FILE__` and the debug info bake absolute source paths into the
# objects, and they survive into a release. Measured 2026-09-13 on the `.nro`:
# 28 such paths, each carrying the build machine's username - all from a
# prebuilt libopus. Our own objects had none, but only because of how asserts
# happen to be compiled, which is luck rather than a decision.
#
# `-ffile-prefix-map` rewrites the prefix at compile time, so `__FILE__` reads
# `core/media/audio.c` instead of a home directory. GCC 8+ and Clang 10+.
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)

# project info
project(halyard)
# === THE VERSION IS THE TAG (2026-09-26) ================================
#
# A release is a `vX.Y.Z` git tag, and nothing else: every package reads its
# number from here - the Switch NACP, the Vita's APP_VER and LiveArea, the
# Windows resource, the About screen, the log. It used to be three literals in
# this file, which a release workflow then had to compare with the tag; a tag
# pushed without the edit was refused, and an edit without the tag shipped a
# number nobody could trace.
#
#   on the tag            -> "0.1.0"
#   commits after it      -> "0.1.0-dev" (the hash in build_id.h says which)
#   no tag, a VERSION file -> its X.Y.Z (the release's source bundle, no git)
#   neither               -> "0.0.0-dev" (a shallow or tag-less clone)
#
# `-DHALYARD_VERSION=X.Y.Z` overrides all of it. Read at
# CONFIGURE time: re-run cmake after tagging in an existing build directory.
set(HALYARD_VERSION "" CACHE STRING "X.Y.Z to stamp on every package; empty = the latest vX.Y.Z tag")
set(VERSION_SUFFIX "")
set(_hv "${HALYARD_VERSION}")
if (_hv STREQUAL "")
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --tags --long --match "v[0-9]*.[0-9]*.[0-9]*"
        OUTPUT_VARIABLE _hdesc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _hres)
    if (_hres EQUAL 0 AND _hdesc MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g[0-9a-f]+$")
        set(_hv "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
        if (NOT CMAKE_MATCH_4 EQUAL 0)
            set(VERSION_SUFFIX "-dev")
        endif ()
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
        # The release's source bundle has no git; the release workflow writes
        # the tag's number into VERSION beside it.
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _hv LIMIT_COUNT 1)
        string(STRIP "${_hv}" _hv)
    else ()
        set(_hv "0.0.0")
        set(VERSION_SUFFIX "-dev")
    endif ()
endif ()
if (NOT _hv MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "HALYARD_VERSION must be X.Y.Z, got '${_hv}'")
endif ()
set(VERSION_MAJOR "${CMAKE_MATCH_1}")
set(VERSION_MINOR "${CMAKE_MATCH_2}")
set(VERSION_ALTER "${CMAKE_MATCH_3}")
set(VERSION_BUILD "0")
set(VERSION_STRING "${_hv}${VERSION_SUFFIX}")
message(STATUS "Halyard version: ${VERSION_STRING}")
# S3 2026-08-22 - THE BUILD FINGERPRINT. The version "0.3.0" is frozen: with the
# console in hand there is no way to tell whether the .nro that just launched is
# the one we just pushed (we lost a whole test round trip to that). So we stamp
# in the build time and the current git hash, logged at startup and shown on the
# boot screen. `git` absent or outside a repo -> "nogit", never a failure.
# The fingerprint is REGENERATED ON EVERY COMPILE (not only at `cmake` time):
# otherwise a plain `make` reuses the old timestamp and the fingerprint lies -
# which has already made us test a stale binary twice while believing otherwise.
# The script writes build_id.h; core/version.h includes it and therefore
# depends on its freshness.
#
# S85 2026-08-29 - THREE MACROS, NOT ONE GLUED STRING. The "About" section shows
# the version, the date and the hash on THREE lines. Recomposing them here only
# to split them apart again at runtime (`strrchr(' ')`…) would be a split that
# goes silently wrong the day the timestamp format changes: you would then read
# a truncated date, not an error. So we emit the three atoms separately, and it
# is `version.h` that recomposes SHADOW_BUILD_ID for the log.
#
# The VERSION comes from VERSION_MAJOR/MINOR/ALTER above - the ones that already
# number the shipped package. It used to be written OUT BY HAND a second time in
# === THE PROJECT'S IDENTITY, IN ONE PLACE ==============================
#
# Everything that names the project downstream reads these: the Switch NACP (so
# the homebrew browser shows it), the PSV param.sfo and its LiveArea bubble, and
# the in-app About screen. Declared once so the three can never disagree - the
# defect `version.h` was written to end.
set(APP_TITLE "Halyard")
set(APP_AUTHOR "Wasabules")
set(APP_URL "https://github.com/Wasabules/halyard")
set(APP_LICENSE "GPL-3.0-or-later")
set(APP_DESCRIPTION "Unofficial Shadow PC client for Nintendo Switch and PS Vita")

# main.cpp: two unconnected places, so a guaranteed divergence at the first 0.4.0.
set(SHADOW_BUILD_ID_H ${CMAKE_BINARY_DIR}/build_id.h)
file(WRITE ${CMAKE_BINARY_DIR}/gen_build_id.cmake
"string(TIMESTAMP T \"%Y-%m-%d %H:%M:%S\" UTC)\n\
execute_process(COMMAND git -C \"${CMAKE_CURRENT_SOURCE_DIR}\" rev-parse --short HEAD\n\
                OUTPUT_VARIABLE H OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE R)\n\
if(NOT R EQUAL 0 OR H STREQUAL \"\")\n\
  set(H \"nogit\")\n\
endif()\n\
execute_process(COMMAND git -C \"${CMAKE_CURRENT_SOURCE_DIR}\" status --porcelain --untracked-files=no\n\
                OUTPUT_VARIABLE D OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)\n\
if(NOT D STREQUAL \"\")\n\
  set(H \"\${H}-dirty\")\n\
endif()\n\
execute_process(COMMAND git -C \"${CMAKE_CURRENT_SOURCE_DIR}\" describe --tags --exact-match\n\
                OUTPUT_VARIABLE G OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE RG)\n\
if(NOT RG EQUAL 0 OR G STREQUAL \"\")\n\
  set(G \"\")\n\
endif()\n\
file(WRITE \"${SHADOW_BUILD_ID_H}\"\n\
  \"#define SHADOW_VERSION    \\\"${VERSION_STRING}\\\"\\n\"\n\
  \"#define SHADOW_BUILD_DATE \\\"\${T}Z\\\"\\n\"\n\
  \"#define SHADOW_BUILD_HASH \\\"\${H}\\\"\\n\"\n\
  \"#define SHADOW_BUILD_TAG  \\\"\${G}\\\"\\n\"\n\
  \"#define SHADOW_APP_NAME   \\\"${APP_TITLE}\\\"\\n\"\n\
  \"#define SHADOW_APP_AUTHOR \\\"${APP_AUTHOR}\\\"\\n\"\n\
  \"#define SHADOW_APP_URL    \\\"${APP_URL}\\\"\\n\"\n\
  \"#define SHADOW_APP_LICENSE \\\"${APP_LICENSE}\\\"\\n\"\n\
  \"#define SHADOW_APP_DESC   \\\"${APP_DESCRIPTION}\\\"\\n\")\n")
# === THE FINGERPRINT MUST MAKE ITS READERS DIRTY, NOT MERELY PRECEDE THEM ===
#
# This was an `add_custom_target(... ALL)` with an order-only dependency, and
# that is NOT enough - it is the same defect twice, found twice, on 2026-09-13.
# An order-only edge guarantees the generator RUNS FIRST; it never marks
# anything dirty. Ninja decides what to recompile BEFORE the command rewrites
# the header, so the new hash lands in `build_id.h` and reaches the binary one
# build later. The console then announces a commit whose code it is not
# running, which is exactly the evidence this repo trusts.
#
# It also says why the first fix looked like it worked: it was verified by
# comparing `build_id.h` to `HEAD`. That file is always right. The only witness
# that means anything is the BINARY.
#
# `add_custom_command(OUTPUT ...)` is the form that carries the information: the
# header becomes a real prerequisite, ninja re-stats it after the command
# (`restat`), and the translation unit including it is recompiled only when the
# content actually changed. The second, never-created output is what makes the
# command run on every build.
add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/build_id.h
            ${CMAKE_BINARY_DIR}/build_id.always
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/gen_build_id.cmake
    COMMENT "Empreinte de build (horodatage + hash git)")
add_custom_target(shadow_build_id ALL
    DEPENDS ${CMAKE_BINARY_DIR}/build_id.h)
include_directories(${CMAKE_BINARY_DIR})
# Generate the fingerprint AT `cmake` time too, so that build_id.h exists from
# the very first compile (the custom target refreshes it on every build).
execute_process(COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/gen_build_id.cmake)

set(PACKAGE_NAME "com.halyard.client")
set(PSN_TITLE_ID  "HLYD00001")
# DERIVED, never retyped. The PSV package format wants "MM.mm"; this used to be
# typed by hand as "00.30" beside a VERSION_MAJOR/MINOR/ALTER of 0.3.0 - two
# encodings of one number, so the first bump to 0.4.0 would have shipped a
# package still numbered 00.30, silently. Same lesson as `version.h`: a value
# that is "the same as" another must be COMPUTED from it.
# "MM.mm" has room for two digits after the dot: minor and patch take one each
# (0.1.2 -> "00.12"), and a minor of 10 or more takes both (the patch is then
# not representable, so the build says so). APP_VER must only grow between
# installs, which this order preserves.
if (VERSION_MAJOR LESS 10)
    set(_psn_major "0${VERSION_MAJOR}")
else ()
    set(_psn_major "${VERSION_MAJOR}")
endif ()
if (VERSION_MINOR LESS 10 AND VERSION_ALTER LESS 10)
    set(PSN_VERSION "${_psn_major}.${VERSION_MINOR}${VERSION_ALTER}")
elseif (VERSION_MINOR LESS 100)
    if (VERSION_MINOR LESS 10)
        set(PSN_VERSION "${_psn_major}.0${VERSION_MINOR}")
    else ()
        set(PSN_VERSION "${_psn_major}.${VERSION_MINOR}")
    endif ()
    message(WARNING "PS Vita APP_VER ${PSN_VERSION} cannot carry the patch of ${VERSION_STRING}")
else ()
    message(FATAL_ERROR "PS Vita APP_VER cannot encode ${VERSION_STRING}")
endif ()
# What the console's HOME menu shows. Separate from `project()`, which names
# the build targets and has to stay lowercase: a target called `Halyard` and a
# menu entry called `halyard` are two different mistakes.
set(PROJECT_AUTHOR "${APP_AUTHOR}")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/img/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)

if (USE_LIBROMFS)
    add_libromfs(${PROJECT_NAME} ${PROJECT_RESOURCES})
endif ()

add_subdirectory(third_party)

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

# setting src and include
list(APPEND APP_PLATFORM_INCLUDE
     ${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/include
     ${CMAKE_CURRENT_SOURCE_DIR}
)
# === LIB1 2026-10-02 - `core/` IS A LIBRARY, AND THE BUILD NOW SAYS SO =======
#
# `core/` was globbed into the application target, so "the core does not depend
# on the client" was true but UNVERIFIED - nothing stopped a `#include` from
# reaching up into `clients/`, and nobody would have noticed until a second
# client tried to use it.
#
# Measured before splitting, so the split is a guarantee of something that was
# already the case rather than a repair: zero includes of `clients/` anywhere in
# `core/`, and linked by hand into a 736 KB archive whose only non-library
# undefined symbol was `shadow_link_info` (now weak, see core/protocol/ctrl_session.c).
#
# Two targets instead of one list:
#   halyard-core   every .c and .cpp under core/, and nothing else;
#   the app        clients/ plus a link against halyard-core.
#
# It also collapses a DUPLICATE enumeration: `halyard-cli` globbed core's C
# sources into its own `SHADOW_C_SRC`, so core was listed in two places that
# could drift. Both targets now link the one library.
#
# The per-platform `add_definitions()` and `include_directories()` above are
# DIRECTORY-scoped, so halyard-core inherits them exactly as the app does -
# which is why this is placed here and not earlier.
file(GLOB_RECURSE CORE_SRC core/*.cpp core/*.c)
file(GLOB_RECURSE CLIENT_SRC clients/*.cpp clients/*.c)

# The TARGET is created further down, just before `program_target`: the
# per-platform blocks between here and there are what fill
# `APP_PLATFORM_INCLUDE`, `APP_PLATFORM_OPTION` and `APP_PLATFORM_LIB`, and a
# library created up here would get them EMPTY. Found the honest way - the
# first version of this split was placed here and `core/media/audio.c` stopped
# finding <opus.h>, because that include arrives through the imported target.
#
# MAIN_SRC stays the name the rest of this file and `program_target` use.
set(MAIN_SRC ${CLIENT_SRC})
# === 2026-09-26 - THE WEBRTC PATH IS GONE, ALL OF IT ===
#
# S112 (2026-08-29) deleted the in-house stack (`webrtc.c`, `ice.c`, `sctp.c`,
# `srtp.c`, `dtls.c`, 4,050 lines that nothing had compiled since the May
# pivot). What survived was the libdatachannel arm (`legacy/dc/`, `wss.c`),
# kept for desktop A/B behind an opt-in; it had not been built on any machine
# that did not still carry libjuice, libsrtp and libdatachannel from the spring,
# and nothing it measured had been asked of it since. It went on 2026-09-26,
# with its libraries, its settings entry and the SCTP half of `shadow_input.c`.
# Git history keeps the code; KB.md keeps what we learned from it.

# The development menu bar (clients/borealis/devui/): mouse-driven debug tooling. It has
# no purpose on console - there is no mouse, and the screen is narrow enough as
# it is. So none of its sources is compiled there and SHADOW_DEV_UI is not
# defined: stream_view.cpp's calls then resolve to devui.hpp's empty `inline`
# stubs. The application carries no #if of its own, and the bar costs nothing on
# the Switch.
file(GLOB DEVUI_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/devui/*.cpp)
if (PLATFORM_SWITCH)
    list(REMOVE_ITEM MAIN_SRC ${DEVUI_SRCS})
endif ()

set(PLATFORM_OPTION)
if (PLATFORM_DESKTOP)
    if (WIN32)
        configure_file("${CMAKE_SOURCE_DIR}/clients/borealis/packaging/resource.rc.in" "${CMAKE_BINARY_DIR}/resource.rc")
        list(APPEND MAIN_SRC ${CMAKE_BINARY_DIR}/resource.rc)
        if (MINGW)
            # Static: produces a portable .exe. If you want shared (a faster debug
            # plus vite), passe -DSHADOW_WIN_STATIC=OFF.
            option(SHADOW_WIN_STATIC "Static link libgcc/libstdc++/winpthread" ON)
            if (SHADOW_WIN_STATIC)
                set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static" CACHE STRING "" FORCE)
            endif ()
        endif ()
    elseif (BUNDLE_MACOS_APP)
        list(APPEND MAIN_SRC ${CMAKE_SOURCE_DIR}/clients/borealis/packaging/halyard.icns)
    endif ()

    if (WIN32)
        # ===== Windows desktop (MinGW-w64 UCRT64) =====
        # This build = the native Shadow path only (= what runs on the Switch).
        # The build_linux wolfSSL has no Windows counterpart: wolfSSL (built
        # locally, see below), FFmpeg, curl, jansson, qrencode and opus come
        # from MSYS2 through pkg-config. See WINDOWS_BUILD.md.
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(WOLFSSL  REQUIRED IMPORTED_TARGET wolfssl)
        # === WIN1 2026-09-10 - THE SYSTEM DEPENDENCIES OF A STATIC wolfSSL ===
        #
        # The MSYS2 package is compiled WITHOUT DTLS (`#undef WOLFSSL_DTLS`), so
        # without `wolfSSL_set_dtls_fd_connected` or `EmbedSendTo`: the input
        # channel, which is DTLS despite the file being called
        # `ctrl_audio_dtls.c`, cannot link against it. So we build our own
        # wolfSSL - see docs/WINDOWS_BUILD.md - and point at it through
        # PKG_CONFIG_PATH.
        #
        # But a STATIC archive does not carry its dependencies: the `.pc` has an
        # empty `Libs.private`, and `ld` only resolves an archive against what
        # PRECEDES it on the line. Borealis already pulls in `-lws2_32` well
        # before, so `recvfrom`, `htons` and `inet_pton` stayed unresolved, and
        # `crypt32` (WOLFSSL_SYS_CA_CERTS) n'etait cite nulle part.
        # Attaching them to the TARGET is what places them AFTER wolfssl.
        if (TARGET PkgConfig::WOLFSSL)
            set_property(TARGET PkgConfig::WOLFSSL APPEND PROPERTY
                         INTERFACE_LINK_LIBRARIES ws2_32 crypt32 advapi32 bcrypt)
        endif ()
        pkg_check_modules(LIBCURL  REQUIRED IMPORTED_TARGET libcurl)
        pkg_check_modules(JANSSON  REQUIRED IMPORTED_TARGET jansson)
        pkg_check_modules(OPUS     REQUIRED IMPORTED_TARGET opus)
        pkg_check_modules(QRENCODE REQUIRED IMPORTED_TARGET libqrencode)
        pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswresample libswscale)

        list(APPEND APP_PLATFORM_LIB
            PkgConfig::WOLFSSL
            PkgConfig::LIBCURL
            PkgConfig::JANSSON
            PkgConfig::OPUS
            PkgConfig::QRENCODE
            PkgConfig::AVCODEC
            # Winsock / IP helpers / the CNG RNG (BCryptGenRandom, in wolfssl_rand_switch.c)
            ws2_32 iphlpapi bcrypt
            # OUT-3 2026-09-11 - media/audio_out_win.c: WASAPI goes through COM
            # (CoInitializeEx, CoCreateInstance, CoTaskMemFree). Its GUIDs are
            # defined in that file, so neither uuid nor ksuser is needed. ole32
            # already reached the link through another library; naming it here
            # makes the dependency ours rather than an accident of the link order.
            # avrt: MMCSS for the playback thread (SHADOW_WIN_AUDIO_MMCSS).
            ole32 avrt
            # pthread = winpthreads, supplied by MinGW-w64. m is libc's.
            pthread m
        )
        # We do NOT build halyard-cli on Windows - disabled further down
        # through SHADOW_BUILD_TEST_CLI.
        set(SHADOW_BUILD_TEST_CLI OFF)
    else ()
        # ===== Linux / macOS desktop =====
        # wolfSSL compiled into third_party/ (tools/bootstrap-libs.sh): the
        # control channel, the DTLS input channel, chacha20-poly1305.
        set(SHADOW_WOLFSSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wolfssl)
        list(APPEND APP_PLATFORM_INCLUDE
            ${SHADOW_WOLFSSL_DIR}
            ${SHADOW_WOLFSSL_DIR}/build_linux
            /usr/include/opus
        )
        # SHADOW_REST_TLS_OPENSSL: the TLS library libcurl carries here, which
        # crypto_selftest.c benches (it links libcrypto just below).
        add_definitions(-DNO_WRITEV -DSINGLE_THREADED -DNO_WOLFSSL_DIR -DSHADOW_REST_TLS_OPENSSL=1)
        list(APPEND APP_PLATFORM_LIB
            ${SHADOW_WOLFSSL_DIR}/build_linux/libwolfssl.a
            # OpenSSL: the system libcurl's TLS stack, which crypto_selftest.c
            # benchmarks directly (libcrypto's bignum/EC).
            ssl crypto
            # FFmpeg + opus + curl + jansson Linux + ALSA (output audio)
            avformat avcodec swresample swscale avutil
            opus curl jansson
            asound
            # libqrencode for the Device Grant QR code (= the login UX)
            qrencode
            pthread m
        )
        set(SHADOW_BUILD_TEST_CLI ON)
    endif ()
elseif (PLATFORM_PSV)
    # === PS VITA ===
    #
    # The SDK's own portlibs supply opus, zstd and Mbed TLS (`vdpm`). Built out
    # of band by tools/build-libs.sh vita: wolfSSL - with the SAME flags the
    # Switch needs: without NO_WRITEV its headers pull <sys/uio.h>, which does
    # not exist here - libcurl and FFmpeg (below).
    #
    # NOTHING HERE HAS RUN ON HARDWARE. It builds; that is the whole claim.
    set(SHADOW_WOLFSSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wolfssl)
    list(APPEND APP_PLATFORM_INCLUDE
        ${SHADOW_WOLFSSL_DIR}
        ${SHADOW_WOLFSSL_DIR}/build_vita
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/build_vita/install/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/build_vita/install/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jansson/build_vita/include
        # opus ships its headers under include/opus/, and `audio.c` asks for
        # <opus.h> - the same second path the Switch build adds.
        $ENV{VITASDK}/arm-vita-eabi/include/opus
    )
    # SHADOW_REST_TLS_MBEDTLS: which TLS library libcurl carries here - read by
    # crypto_selftest.c, which benches that library.
    add_definitions(-DNO_WRITEV -DNO_WOLFSSL_DIR -DSHADOW_REST_TLS_MBEDTLS=1)
    set(APP_PLATFORM_LIB
        ${SHADOW_WOLFSSL_DIR}/build_vita/libwolfssl.a
        # === libcurl ON MBED TLS, NOT THE SDK'S (2026-09-26) ================
        #
        # The vitasdk's libcurl is built on OpenSSL 1.0.2 and linked
        # statically: GPL code (ours, wolfSSL) with the OpenSSL 1.x licence,
        # whose advertising clause the GPL does not allow. The .vpk could not
        # be redistributed. tools/build-libs.sh rebuilds the SAME curl (8.17.0,
        # the vitasdk's own options) on the SDK's Mbed TLS 3.6.5 (Apache-2.0);
        # the full path keeps the SDK's `-lcurl` from being picked instead, and
        # no `ssl crypto` follows it any more. wolfSSL stays built without
        # `opensslextra`: our code calls the native `wolfSSL_*` names.
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/build_vita/install/lib/libcurl.a
        mbedtls mbedx509 mbedcrypto
        # jansson comes from third_party, NOT from the SDK: the vitasdk package
        # is built with -fPIC, and `vita-elf-create` refuses the GOT-relative
        # relocations that produces ("Invalid relocation type 25"). It was the
        # last thing between a linked ELF and a .vpk. Rebuilt without PIC by
        # tools/bootstrap-libs.sh.
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jansson/build_vita/lib/libjansson.a
        opus
        # === FFmpeg: OURS, FLAC DECODER ONLY (2026-09-26) ====================
        # The vitasdk's FFmpeg is LGPL and drags LAME and mpg123 (LGPL) in with
        # it; shipping them means shipping their exact source, and vdpm serves
        # whatever version is current - the release could not name it. Video is
        # SceAvcdec here, so libavcodec only decodes FLAC audio: tools/build-libs.sh
        # vita ffmpeg builds that from the tarball the Switch pins. `FLAC ogg
        # swresample mp3lame mpg123` used to sit on this line; the first three
        # contributed nothing, the last two only what the SDK's FFmpeg needed.
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/build_vita/install/lib/libavcodec.a
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/build_vita/install/lib/libavutil.a
        # zstd: libcurl's content decoding (as in the vitasdk's own curl)
        zstd z m
        SceAudio_stub SceNet_stub SceNetCtl_stub SceRtc_stub SceKernelDmacMgr_stub
        # SceVideodec carries sceAvcdec* too - there is no separate SceAvcdec
        # stub, whatever the header split suggests. Linked already although no
        # Vita video decoder is written yet: the day one is, the link line is
        # not what should have to change.
        SceVideodec_stub
        ${CMAKE_THREAD_LIBS_INIT}
    )

elseif (PLATFORM_SWITCH)
    # The WebRTC stack for Shadow streaming (M7+) - libs cross-compiled into
    # third_party/. CMake does not drive their build; we just add the .a files
    # already produced plus their include paths. See 03-shadow-recon/WEBRTC_PLAN.md
    # for the roadmap.
    set(SHADOW_WOLFSSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wolfssl)
    set(SHADOW_LIBOPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libopus)
    # M15: libusrsctp (the data channel) - not cross-compiled yet.
    list(APPEND APP_PLATFORM_INCLUDE
        # wolfSSL: DTLS 1.2 (M11) + TLS 1.3 for WSS (M8) + ChaCha20
        ${SHADOW_WOLFSSL_DIR}
        ${SHADOW_WOLFSSL_DIR}/build_switch
        # libopus : audio decode (M14)
        ${SHADOW_LIBOPUS_DIR}/include
        ${SHADOW_LIBOPUS_DIR}/include/opus
    )
    # The same flags that were passed to the wolfSSL build - otherwise ssl.h
    # pulls in <sys/uio.h> / pthread / opendir, which do not exist on the Switch.
    add_definitions(-DNO_WRITEV -DSINGLE_THREADED -DNO_WOLFSSL_DIR)
    # _POSIX_THREADS to enable pthread.h - newlib gates its APIs behind it
    add_definitions(-D_POSIX_THREADS=1 -D_REENTRANT)

    set(APP_PLATFORM_LIB
        # HTTP / JSON. devkitPro's libcurl talks to the console's own TLS
        # service (`Curl_ssl_libnx`, over IPC): no TLS library is linked for it.
        # `mbedtls mbedx509 mbedcrypto` used to follow, contributing not one
        # symbol (checked 2026-09-26: 0 `mbedtls_` in the ELF).
        curl z jansson
        # 2026-09-26 - libjuice and libsrtp2 used to be linked here: the WebRTC
        # stack of M7-M12, abandoned for the native protocol in May. No code
        # compiled for the Switch calls either, nothing builds them for this
        # target, and a machine that had built them long ago kept linking the
        # stale archives - so the only build that failed was a fresh clone's.
        ${SHADOW_LIBOPUS_DIR}/build_switch/libopus.a
        ${SHADOW_WOLFSSL_DIR}/build_switch/libwolfssl.a
        # FFmpeg 7.1.5 + the nvtegra hwaccel (NVDEC through the VIC), built by
        # tools/build-ffmpeg-switch.sh. It is configured --disable-autodetect
        # with no external library, so it needs nothing else from portlibs.
        # 2026-09-26 - `envideo stdc++` and `dav1d ass fribidi harfbuzz
        # freetype png16 bz2` used to follow: the needs of Averne's Envideo
        # generation and of the old dkp switch-ffmpeg package. Neither is what
        # the .nro links; not one symbol came from them.
        avformat avcodec swresample swscale avutil
        # pthread (our DTLS dispatch code is multi-threaded)
        pthread
        # needed by borealis
        glfw3 EGL glapi drm_nouveau
        # base lib
        nx m
    )
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
elseif (PLATFORM_PS4)
    # Show backtrace when app crash
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/ps4/crashlog.c)
elseif (PLATFORM_PSV)
    add_definitions(-D__psp2__ -D__PSV__)
endif ()


# building target
add_library(halyard-core STATIC ${CORE_SRC})
target_include_directories(halyard-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
# PUBLIC so a client that links the library gets them too, and so the app keeps
# compiling unchanged.
target_include_directories(halyard-core PUBLIC ${APP_PLATFORM_INCLUDE})
target_compile_options(halyard-core PRIVATE
    -ffunction-sections -fdata-sections ${APP_PLATFORM_OPTION})
# Same warning policy as the app, and for the same reason: C is where the
# protocol lives. Not -Werror - that belongs to the test runner, which compiles
# the pure modules with it.
target_compile_options(halyard-core PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)

# LIB1: PUBLIC, and this is the architectural point of the split - `core/` is
# what includes <opus.h>, <wolfssl/*>, <libavcodec/*>, <curl/*> and <jansson.h>,
# not the client. Declaring them on the library gives every consumer - the app,
# halyard-cli, a future Qt or Tauri client - the right include paths and the
# right link line from one place, instead of each one rediscovering them.
target_link_libraries(halyard-core PUBLIC ${APP_PLATFORM_LIB})

program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
if (PLATFORM_DESKTOP)
    # The dev menu bar is DESKTOP tooling: it is mouse-driven and speaks GLFW
    # directly. `NOT PLATFORM_SWITCH` used to stand in for "desktop", which held
    # while there were two targets; the PS Vita has neither a mouse nor GLFW.
    # Stays on the APP: the development menu bar is `clients/borealis/devui/`,
    # and nothing in core/ reads this (LIB1).
    target_compile_definitions(${PROJECT_NAME} PRIVATE SHADOW_DEV_UI)
endif ()


# building release file
if (PLATFORM_DESKTOP)
    if (BUNDLE_MACOS_APP)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME ${PROJECT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}"
            MACOSX_BUNDLE_LONG_VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}"
            MACOSX_BUNDLE_ICON_FILE "halyard.icns"
            MACOSX_BUNDLE_COPYRIGHT "Copyright 2023 ${PROJECT_AUTHOR}"
            RESOURCE "${CMAKE_SOURCE_DIR}/clients/borealis/packaging/halyard.icns"
        )
        add_custom_target(${PROJECT_NAME}.data
                COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${PROJECT_NAME}.app/Contents/Resources/resources
        )
    else ()
        add_custom_target(${PROJECT_NAME}.data
                COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_CURRENT_BINARY_DIR}/resources
        )
    endif ()
    if (NOT USE_LIBROMFS)
        add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}.data)
    endif ()
elseif (PLATFORM_PSV)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12") # max heap size mode
    # (2026-09-26 - an `openssl-asm` block used to sit here: it put an OpenSSL
    # 1.0.2i rebuilt WITH the ARM bignum assembly ahead of the SDK's, to cut
    # the 402-1846 ms TLS handshakes. OpenSSL is no longer linked on the Vita
    # at all - libcurl is on Mbed TLS - so the block and its script went.)
    # === ROOM FOR THE SCE METADATA AT THE END OF THE CODE SEGMENT ==========
    #
    # `vita-elf-create` appends a few KB of SCE data after segment 0, and it
    # needs the gap between the end of the code and the start of the data to
    # hold it. With the default 4 KB page alignment that gap is whatever the
    # rounding happens to leave: it was 20 484 bytes, the binary grew by about
    # 5 KB on 2026-09-13, and it became 444 - against the 3 368 required.
    #
    #   vita-elf-create: Cannot allocate 3368 bytes for SCE data at end of
    #   segment 0; segment 1 overlaps
    #   Segmentation fault
    #
    # The vitasdk's default linker script starts the data on the next 64 KB
    # boundary; nothing here sets that (an earlier version of this comment said
    # it did). So the gap was a dice roll on the code size - and on 2026-09-26 a
    # FRESH vitasdk, the one the CI installs, rolled 2 084 bytes: the release
    # job would have failed. `psv/sce_gap.ld` now pads the end of the code
    # segment whenever the gap would be under 8 KB, so the room no longer
    # depends on the binary's size. The check below stays, and the deploy tool
    # still refuses a `.self` older than its ELF: this failure once went
    # unnoticed for three deployments because the message contains no
    # "error:" and the build's exit code was read through a `grep`.
    target_link_options(${PROJECT_NAME} PRIVATE
            "-Wl,-T,${CMAKE_SOURCE_DIR}/clients/borealis/psv/sce_gap.ld")
    # And the gap is CHECKED rather than hoped for. `vita-elf-create` prints a
    # message with no "error:" in it and then segfaults when the room is short,
    # leaving the previous `.self` in place - which is how three deployments in
    # a row shipped the wrong build. This runs on the freshly linked ELF and
    # fails the build with both numbers.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E env "PATH=$ENV{VITASDK}/bin:$ENV{PATH}"
                    python3 ${CMAKE_SOURCE_DIR}/tools/vita-check-gap.py
                    $<TARGET_FILE:${PROJECT_NAME}>
            COMMENT "Checking the SCE metadata will fit after the code segment")
    vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
    # The LiveArea bubble's text comes from the identity block, not from a
    # second copy typed into the XML. `sce_sys` is copied into the build tree so
    # the generated template.xml sits beside the images the package expects.
    set(SHADOW_VERSION_STR "${VERSION_STRING}")
    set(PSV_SCE_SYS ${CMAKE_BINARY_DIR}/sce_sys)
    file(COPY ${CMAKE_SOURCE_DIR}/clients/borealis/psv/sce_sys/ DESTINATION ${PSV_SCE_SYS})
    file(REMOVE ${PSV_SCE_SYS}/livearea/contents/template.xml.in)
    configure_file(
        ${CMAKE_SOURCE_DIR}/clients/borealis/psv/sce_sys/livearea/contents/template.xml.in
        ${PSV_SCE_SYS}/livearea/contents/template.xml @ONLY)
    set(PSV_ASSETS_FILES ${PSV_SCE_SYS} sce_sys)
    if (NOT USE_LIBROMFS)
        list(APPEND PSV_ASSETS_FILES ${CMAKE_SOURCE_DIR}/resources resources)
    endif ()
    if (NOT USE_GXM)
        list(APPEND PSV_ASSETS_FILES ${CMAKE_SOURCE_DIR}/clients/borealis/psv/module/ module)
    endif ()
    if (USE_VITA_SHARK)
        list(APPEND PSV_ASSETS_FILES "${CMAKE_BINARY_DIR}/vendor/SceShaccCg" module)
    endif ()
    vita_create_vpk(${PROJECT_NAME}.vpk ${PSN_TITLE_ID} ${PROJECT_NAME}.self
            VERSION ${PSN_VERSION}
            NAME ${APP_TITLE}
            FILE ${PSV_ASSETS_FILES}
    )
elseif (PLATFORM_PS4)
    # There is no `add_self` and `add_pkg` functions without pacbrew toolchain.
    # We can install the OpenOrbits SDK so that the IDE can provide some basic code prompts,
    # but compilation is done through the pacbrew Docker container as pacbrew only supports Linux.
    if (PS4)
        # Defined by pacbrew toolchain
        add_self(${PROJECT_NAME})
        add_pkg(${PROJECT_NAME}
                # Put all the resources inside `<build dir>/ps4` directory, and access through `/app0/...`
                # eg: `cmake-build/ps4/resources/icon.png` -> `/app0/resources/icon.png`
                ${CMAKE_BINARY_DIR}/ps4
                ${PSN_TITLE_ID}
                "${APP_TITLE}"
                ${PSN_VERSION}
                # https://psdevwiki.com/ps4/param.sfo#ATTRIBUTE
                # Support ps4 pro
                8388608
        )
    endif()
    add_custom_target(${PROJECT_NAME}.data
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/clients/borealis/ps4 ${CMAKE_BINARY_DIR}/ps4
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/ps4/resources
    )
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}.data)
    add_dependencies(${PROJECT_NAME} shadow_build_id)
elseif (PLATFORM_SWITCH)
    if (USE_DEKO3D)
        gen_dksh("${PROJECT_RESOURCES}/shaders")
    endif ()
    # === S100 2026-08-29 - THE FINGERPRINT WAS NOT REGENERATING ON SWITCH ===
    #
    # `shadow_build_id` is declared `ALL`, which builds it along with the default
    # target - but NOT when a named target is requested, and the Switch flow
    # always asks for `halyard.nro`. The desktop branches declared the
    # dependency explicitly; this one had forgotten it.
    #
    # The exact consequence, as reported by the user: the "About" screen showed
    # a timestamp hours old and the hash of a superseded commit, while the rest
    # of the binary was up to date. A wrong build fingerprint is worse than no
    # fingerprint - it makes you believe you are testing a version you are not,
    # which is precisely what it exists to prevent.
    add_dependencies(${PROJECT_NAME} shadow_build_id)

    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${APP_TITLE}" "${PROJECT_AUTHOR}" "${VERSION_STRING}" ${PROJECT_NAME}.nacp --titleid=${PROJECT_TITLEID}
        # REMOVE FIRST. `copy_directory` copies, it never deletes: an asset
        # dropped from the source tree stays in the build tree and keeps
        # shipping in the romfs. Found 2026-09-13 - 18 Pokemon sprites deleted
        # from `resources/` were still inside the `.nro` afterwards.
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/font
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
elseif (PLATFORM_IOS)
    ios_bundle(
            "${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/ios/iphoneos/Splash.storyboard"
            "${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/ios/Images.xcassets"
            "${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/ios/iOSBundleInfo.plist.in"
            "borealis"
            "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}")
elseif (PLATFORM_TVOS)
    ios_bundle(
            "${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/ios/tvos/Splash.storyboard"
            "${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/ios/Images.xcassets"
            "${CMAKE_CURRENT_SOURCE_DIR}/clients/borealis/ios/iOSBundleInfo.plist.in"
            "borealis"
            "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}")
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE . ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections ${APP_PLATFORM_OPTION})
target_link_libraries(${PROJECT_NAME} PRIVATE halyard-core borealis ${APP_PLATFORM_LIB})
# LIB1: the library is listed FIRST. A static archive only contributes the
# objects that something already references, and the app is what references
# the core - borealis and the platform libraries then satisfy what the core
# needs. The reverse order drops symbols on a linker that does one pass.

# Compiler warnings: they were DISABLED, so every new defect went by in silence
# (the first pass found a real one: a NACK rate limit whose variable was never
# read back).
# Restricted to C: that is where the protocol lives, and our C++ files pull in
# borealis's headers, which produce ~460 warnings that are not ours to fix.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra>)

# === Sanitizers (Linux desktop debug only) ===
# Enable it with:  cmake -DSHADOW_ASAN=ON .
# AddressSanitizer + UBSan detect: use-after-free, double-free, heap/stack
# overflow, leaks, undefined behaviour. ~2x slower, but it shows EXACTLY
# where the bug happens (with a symbolised stack trace).
option(SHADOW_ASAN "Enable AddressSanitizer + UBSan (Linux only, debug)" OFF)
if (SHADOW_ASAN AND PLATFORM_DESKTOP AND NOT WIN32)
    message(STATUS "ASan + UBSan ENABLED — binary will be slower but bugs visible")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(${PROJECT_NAME} PRIVATE
        -fsanitize=address,undefined)
    if (TARGET halyard-cli)
        target_compile_options(halyard-cli PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(halyard-cli PRIVATE
            -fsanitize=address,undefined)
    endif()
endif()

# === halyard-cli - the headless benchmark binary (desktop only) ===
# Reuses the pure C sources of core/ without Borealis, to automate the freeze
# tests from a terminal.
# Skipped on Windows (SHADOW_BUILD_TEST_CLI=OFF): it used to need wss.c and
# libdatachannel symbols MinGW did not have. Those are gone since 2026-09-26;
# whether it now builds there has not been tried.
if (PLATFORM_DESKTOP AND SHADOW_BUILD_TEST_CLI)
    # LIB1: links the library instead of re-globbing core. The old
    # `SHADOW_C_SRC` listed five directories by hand and had already drifted -
    # it enumerated `core/*/` while the app used GLOB_RECURSE, so a new
    # subdirectory would have reached the app and not the CLI.
    #
    # This is also the smallest possible PROOF that the library is usable from
    # outside the Borealis app: halyard-cli links it, provides the one weak
    # symbol it wants, and draws nothing from `clients/`.
    add_executable(halyard-cli cli/main_test.c cli/test_cli_stubs.c)
    target_link_libraries(halyard-cli PRIVATE halyard-core ${APP_PLATFORM_LIB})
    target_compile_options(halyard-cli PRIVATE -ffunction-sections -fdata-sections)
endif ()

# === QT1 2026-10-03 - THE DESKTOP CLIENT, ON `halyard-core` =================
#
# A second client for Linux, Windows and macOS. `clients/qt/README.md` holds the
# reasoning; the short version is that the video path carries 187 MB/s (1080p
# NV12 at 60 fps), which rules out anything crossing a webview IPC bridge, and
# that `halyard-core` is C so Qt needs no FFI layer.
#
# Qt 6.8 LTS as the FLOOR, not as a pin. 6.8 is the current LTS with standard
# support to 2029-10-08; the next is 6.12 (one every four minor versions). The
# build then uses whatever the platform ships - MSYS2 UCRT64 currently has
# 6.11.0, which is a regular release. Pinning to a non-LTS version would put an
# expiry date on the build for nothing.
#
# Multimedia is needed as well as Widgets: `QVideoFrame`/`QVideoSink` are what
# take NV12 and YUV420P and convert on the GPU.
#
# Desktop only and opt-in: the consoles have no business carrying Qt, and a
# developer who has not installed it should not have their build fail.
option(SHADOW_BUILD_QT "Build the Qt desktop client (needs Qt 6.8+)" OFF)
if (SHADOW_BUILD_QT AND PLATFORM_DESKTOP)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Concurrent
                 LinguistTools)
    qt_standard_project_setup()

    # QT2 2026-10-03 - headers are listed so AUTOMOC sees them: every one of
    # these carries Q_OBJECT, and a missing moc file is a link error about a
    # vtable, far from its cause.
    qt_add_executable(halyard-qt
        clients/qt/main.cpp
        clients/qt/main_window.cpp          clients/qt/main_window.hpp
        clients/qt/auth_worker.cpp          clients/qt/auth_worker.hpp
        clients/qt/bootstrap_worker.cpp     clients/qt/bootstrap_worker.hpp
        clients/qt/session_worker.cpp       clients/qt/session_worker.hpp
        clients/qt/step_list_widget.cpp     clients/qt/step_list_widget.hpp
        clients/qt/video_widget.cpp         clients/qt/video_widget.hpp
        clients/qt/settings_window.cpp      clients/qt/settings_window.hpp
        clients/qt/metrics_window.cpp       clients/qt/metrics_window.hpp
        clients/qt/about_dialog.cpp         clients/qt/about_dialog.hpp
        clients/qt/theme.cpp                clients/qt/theme.hpp
        clients/qt/i18n.cpp                 clients/qt/i18n.hpp
        clients/qt/settings_store.cpp       clients/qt/settings_store.hpp
        clients/qt/ft_worker.cpp            clients/qt/ft_worker.hpp
        clients/qt/file_manager_window.cpp  clients/qt/file_manager_window.hpp
        clients/qt/i18n_match.hpp
        clients/qt/core_scope.hpp
        clients/qt/settings_model.hpp
        clients/qt/plane_copy.hpp
        clients/qt/stream_overlay.cpp       clients/qt/stream_overlay.hpp
        clients/qt/overlay_paint.cpp        clients/qt/overlay_paint.hpp
        clients/qt/hud_model.cpp            clients/qt/hud_model.hpp
        clients/qt/machine_card.cpp         clients/qt/machine_card.hpp
        clients/qt/qt_input_map.hpp
        clients/qt/rate_meter.hpp
    )
    # FM1 2026-10-03 - the file manager needs the SFTP channel, which is core's
    # `filetransfer.c` behind -DSHADOW_FILETRANSFER=ON (libssh). A desktop
    # client is exactly the "future desktop client" that module was written for
    # (its own header says so), so the Qt build turns it ON by default - unlike
    # the consoles, which have no business carrying libssh. Pass
    # -DSHADOW_FILETRANSFER=OFF to build the Qt client without it; the menu entry
    # then reports "built without file-transfer support" rather than vanishing.
    if (NOT DEFINED SHADOW_FILETRANSFER OR SHADOW_FILETRANSFER)
        set(SHADOW_FILETRANSFER ON CACHE BOOL "" FORCE)
    endif ()
    # QT5 - C++20 for the settings table's designated initializers: a row of
    # fifteen positional fields is how two of them end up swapped unseen.
    set_target_properties(halyard-qt PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

    # === QT5 2026-10-03 - TRANSLATIONS =======================================
    #
    # Qt Linguist's standard path: `lupdate` extracts tr() and QT_TRANSLATE_NOOP
    # from the sources into the .ts files, `lrelease` compiles them at build
    # time, and the .qm
    # are EMBEDDED under :/i18n - no file to forget next to the binary, which
    # the CA bundle already showed is how a release breaks.
    #
    # RESOURCE_PREFIX must match `kResourceDir` in clients/qt/i18n.cpp: the
    # selector lists what it finds there, so a mismatch does not fail - it
    # silently offers English only.
    #
    # SOURCE_TARGETS is not optional. Since Qt 6.7 the default is EVERY target
    # in the project, and the first run pulled 880 strings, a third of them
    # from Borealis's own `ui::tr()` - a function that merely shares the name.
    #
    # The extraction target is `halyard_lupdate` (or `update_translations`),
    # named after the project, not the executable.
    #
    # Adding a language = adding its .ts here. No code names a language.
    qt_add_translations(halyard-qt
        SOURCE_TARGETS halyard-qt
        TS_FILES clients/qt/i18n/halyard_fr.ts
        RESOURCE_PREFIX /i18n
        LUPDATE_OPTIONS -no-obsolete -locations none)
    # halyard-core, and NOT borealis: this client shares the library and nothing
    # else. `tools/check-core-independence.py` keeps core from reaching into
    # either client; this link line is what keeps the two clients apart.
    target_link_libraries(halyard-qt PRIVATE
        halyard-core Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets
        # QT2: Concurrent for the one short blocking call that does not justify
        # a worker of its own (the machine list).
        Qt6::Concurrent)
    target_include_directories(halyard-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(halyard-qt PROPERTIES WIN32_EXECUTABLE OFF)
    # WIN32_EXECUTABLE OFF on purpose while this is a skeleton: a console
    # subsystem binary shows qDebug/qCritical, and the first thing being
    # debugged here is a thread contract.
    # === QT2 2026-10-03 - THE CA BUNDLE TRAVELS WITH THE BINARY ============
    #
    # `core/services/http.c` (WIN2) points the desktop at `resources/cacert.pem`
    # RELATIVE TO THE WORKING DIRECTORY, because MSYS2's libcurl derives its own
    # CA path from its DLL's directory and that is wrong once the DLLs sit next
    # to the executable.
    #
    # The Borealis build happens to copy `resources/` as part of its asset step,
    # so it never noticed. This target has no asset step, so the first run of
    # halyard-qt failed EVERY verified request with
    # "Problem with the SSL CA cert" - the same defect as WIN2, in a new build
    # directory, for the same reason the test runner found TMP again.
    #
    # Copied at build time rather than at install: a developer runs the binary
    # out of the build directory, which is where it has to work.
    add_custom_command(TARGET halyard-qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:halyard-qt>/resources
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/resources/cacert.pem
                $<TARGET_FILE_DIR:halyard-qt>/resources/cacert.pem
        COMMENT "halyard-qt: the CA bundle next to the binary (QT2/WIN2)")

    message(STATUS "Qt client: ON (Qt ${Qt6_VERSION}, floor 6.8 LTS)")
endif ()

# === LIB1 2026-10-02 - PROOF THAT THE LIBRARY IS USABLE FROM OUTSIDE ========
#
# `examples/minimal_client.c` links `halyard-core` and nothing else, provides no
# client symbol but a frame sink, and runs. It is the linker's answer to the
# same question `tools/check-core-independence.py` asks of the source: has
# `core/` grown a dependency on a client?
#
# Desktop only and opt-in. It is a build check and a worked example for whoever
# starts the Qt or Tauri client, not something a console image should carry.
option(SHADOW_BUILD_EXAMPLES "Build the examples/ consumers of halyard-core" OFF)
if (SHADOW_BUILD_EXAMPLES AND PLATFORM_DESKTOP)
    add_executable(halyard-minimal examples/minimal_client.c)
    # halyard-core ONLY: no borealis, no APP_PLATFORM_LIB of its own. Anything
    # it needs beyond the library comes through the library's PUBLIC link, which
    # is the property being checked.
    target_link_libraries(halyard-minimal PRIVATE halyard-core)
    message(STATUS "examples: ON (halyard-minimal)")
endif ()

# === FT1 2026-10-02 - the file-transfer channel, OFF by default ==============
#
# `core/services/filetransfer.c` speaks SFTP to the VM on `:base+15` (KB §3.50).
# It is for a FUTURE desktop client (Qt/Tauri); the homebrew UI does not expose
# it and nothing in `clients/` calls it. libssh is a real dependency, and the
# consoles have no business carrying it, so this is opt-in:
#
#   cmake -DSHADOW_FILETRANSFER=ON ...
#
# With the option OFF the .c still compiles - `file(GLOB_RECURSE core/*.c)`
# sweeps it up on every target - but its body is behind
# `SHADOW_HAVE_FILETRANSFER` and every entry point returns
# SHADOW_FT_UNSUPPORTED. One nearly empty translation unit, no dependency, and
# the secret-extraction half (pure protobuf) stays available everywhere.
option(SHADOW_FILETRANSFER "Build the SFTP file-transfer client (needs libssh)" OFF)
if (SHADOW_FILETRANSFER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh)
    # LIB1: on the LIBRARY, because `filetransfer.c` lives there. PUBLIC on the
    # link so every consumer of halyard-core gets libssh, and PUBLIC on the
    # define so a client can test `SHADOW_HAVE_FILETRANSFER` and grey out its
    # own file-transfer UI instead of calling into a stub.
    target_compile_definitions(halyard-core PUBLIC SHADOW_HAVE_FILETRANSFER=1)
    target_link_libraries(halyard-core PUBLIC PkgConfig::LIBSSH)
    message(STATUS "file transfer: ON (libssh ${LIBSSH_VERSION})")
endif ()

# ---------------------------------------------------------------------------
# Offline tests of the protocol logic (`make tests`).
# They need no console, no VM and no network: they check the PURE functions of
# core/protocol/vid_wire.c, the ones whose every bug cost a full RE
# campaign (G4/G21, G40, G43). Run them before touching the video path.
# === THE BUILD FINGERPRINT MUST PRECEDE THE COMPILE ========================
#
# `shadow_build_id` regenerates `build_id.h` (timestamp + git hash), and the
# ordering used to be declared in ONE place: inside the Switch/desktop branch's
# `if (NOT USE_LIBROMFS)`, mis-indented, which is how a line ends up somewhere
# it was never meant to be. Two consequences, and the second is the expensive
# one: with libromfs on, no platform ordered it; and the PSV branch never
# reaches that code at all, so on the Vita there was no dependency whatsoever.
#
# The target is `ALL`, so it always RAN - just not necessarily before the
# translation unit that includes the header. The fingerprint therefore lagged
# one build, and on 2026-09-13 that cost a real misreading: a console log
# announced `af0a8a1 02:04:52Z` while running code committed two hours later,
# which is exactly the kind of evidence this repo trusts. A fingerprint that
# can be wrong is worse than none - it is read as proof of which binary ran.
#
# Declared here, once, unconditionally, after every target exists.
add_dependencies(${PROJECT_NAME} shadow_build_id)
if (TARGET halyard-cli)
    add_dependencies(halyard-cli shadow_build_id)
endif ()

# ---------------------------------------------------------------------------
add_custom_target(tests
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Tests hors ligne — semantique du fil video"
    USES_TERMINAL)
