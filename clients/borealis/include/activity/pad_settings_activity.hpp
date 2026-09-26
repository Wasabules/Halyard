/* The Gamepad screen - built on the in-house framework.
 *
 * Button mappings, vertical axis inversion, dead zone, reset - and a TESTER that
 * draws TWO controllers: what the console reads, and what we actually send. See
 * pad_view.hpp for why those two.
 */
#pragma once

#include <borealis.hpp>

class PadView;

class PadSettingsActivity : public brls::Activity {
public:
    brls::View *createContentView() override;
    void onContentAvailable() override;

private:
    PadView *view = nullptr;
};
