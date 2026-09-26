/* The "Log" screen (S86) - the activity only wires the buttons up. All the
 * display and all the state live in JournalView; its header explains why this
 * screen is not a `ui::ListScreen`. */
#pragma once

#include <borealis.hpp>

class JournalView;

class JournalActivity : public brls::Activity
{
public:
    brls::View *createContentView() override;
    void onContentAvailable() override;

private:
    JournalView *view = nullptr;
};
