/* tests/mock/borealis/core/assets.hpp - stands in for Borealis' header when
 * tests/test_sfx_handoff.cpp compiles ui/sfx.cpp. The application gets
 * BRLS_RESOURCES from its build; the test runs from tests/, so the sounds it
 * loads are the repository's own resources/sfx/ files. */
#pragma once

#ifndef BRLS_RESOURCES
#define BRLS_RESOURCES "../resources/"
#endif
