/* The "Touch gestures" screen - the activity only wires the buttons up. */
#pragma once

#include <borealis.hpp>

class GesturesView;

class GesturesSettingsActivity : public brls::Activity
{
public:
    brls::View *createContentView() override;
    void onContentAvailable() override;

private:
    GesturesView *view = nullptr;
};
