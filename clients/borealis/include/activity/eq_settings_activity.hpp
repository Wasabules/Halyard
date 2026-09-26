/* Screen Egaliseur — cf. gestures_settings_activity, meme structure. */
#pragma once

#include <borealis.hpp>

class EqView;

class EqSettingsActivity : public brls::Activity
{
public:
    brls::View *createContentView() override;
    void onContentAvailable() override;

private:
    EqView *view = nullptr;
};
