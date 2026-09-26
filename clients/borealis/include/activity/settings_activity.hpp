/* The Settings screen - the single entry point for the application's settings.
 *
 * It has neither XML nor Borealis widgets any more: its content is a single
 * view, `SettingsView`, drawn by the in-house framework. See its header for the
 * four defects that motivated the port (toggle states overwritten by their
 * description, "ON"/"OFF" left in English, text that overflowed, and a B-button
 * return that crashed).
 */
#pragma once

#include <borealis.hpp>

/* Declared, not included: this header only needs a pointer, and including
 * it would tie clients/borealis/include/ to . */
class SettingsView;

class SettingsActivity : public brls::Activity {
public:
    brls::View *createContentView() override;

    void onContentAvailable() override;
    void onResume() override;

private:
    SettingsView *view = nullptr;
};
