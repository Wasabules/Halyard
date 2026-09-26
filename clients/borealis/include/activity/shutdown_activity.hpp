/* ShutdownActivity - the shutdown screen.
 *
 * === UX10 2026-09-10 ===
 *
 * See `ui/shutdown.hpp` for the measurement behind this screen: the window
 * vanished while the application kept working for three more seconds - up to a
 * minute with the network down - and nothing said so.
 *
 * It is pushed ON TOP of everything else when the shutdown is requested, and
 * gives control back only once the shutdown tasks have withdrawn. It cancels
 * nothing: the shutdown is already decided when it appears.
 */
#pragma once

#include <borealis.hpp>

class ShutdownView;

class ShutdownActivity : public brls::Activity {
public:
    brls::View *createContentView() override;

private:
    ShutdownView *view = nullptr;
};
