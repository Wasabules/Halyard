// StreamActivity — full-screen video stream display.
// Hosts a StreamView as its content view. Pushed by MainActivity once
// the WebRTC session is connected and decoding.
#pragma once

#include <borealis.hpp>
#include "clients/borealis/activity/stream_view.hpp"

class StreamActivity : public brls::Activity {
public:
    StreamActivity();
    ~StreamActivity() override;

    void onContentAvailable() override;

    brls::View* createContentView() override;

private:
    StreamView* streamView = nullptr;
};
