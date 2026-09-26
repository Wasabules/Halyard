/* The Stream Quality screen - built on the in-house framework.
 *
 * Five cycling choices (bitrate, frames per second, resolution, codec, profile).
 * No more XML: the activity returns its view, which fills the whole screen.
 */
#pragma once

#include <borealis.hpp>

class QualityView;

class QualitySettingsActivity : public brls::Activity {
public:
    brls::View *createContentView() override;
    void onContentAvailable() override;

private:
    QualityView *view = nullptr;
};
