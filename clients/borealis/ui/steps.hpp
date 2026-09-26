/* ui::StepScreen - the framework's SECOND archetype: a progress page.
 *
 * Two screens needed it - boot and connect - and both show the same thing: a
 * sequence of steps whose progress you follow, with a message and sometimes a
 * retry action. Until now they were Borealis frames wrapping a hand-written
 * view; the wrapper existed solely to place a title and a footer, which the
 * framework knows how to do.
 *
 * Like `ListScreen`, it holds NO view and NO pointer: a step is a struct of
 * values, and the screen is redrawn from them. It has no focus at all - you do
 * not navigate a progress page, you watch it. That is what makes it immune by
 * construction to the family of crashes that cost us that whole day.
 *
 * The screen knows nothing about the network or about threads: the background
 * work PUSHES its progress in (`setEtapes`), it never reads anything from here.
 * That is what stops an async callback from holding a reference to UI.
 *
 * === S80 2026-08-29 - ONE RING, AND STEPS THAT ACCUMULATE ===
 *
 * The first version laid out seven bulleted rows, all visible from the first
 * frame. It said everything - and therefore showed nothing: seven identical grey
 * dots do not answer the question you actually ask in front of a loading screen,
 * which is "where are we, and is it moving?". You had to read and count to find
 * out.
 *
 * The ring answers that at a glance: its filled portion IS the progress, and it
 * moves. The steps, for their part, APPEAR one after another - the remaining
 * ones are only sketched in. So you watch what is done pile up, instead of
 * watching a frozen list colour itself in from the middle.
 *
 * HOW THE ANIMATION WORKS WITHOUT SESSION STATE. `setEtapes` is called EVERY
 * FRAME by ConnectingView, which re-reads its model - a start instant stored
 * inside the step would therefore be reset sixty times a second, and nothing
 * would ever move. So the appearance instant is kept apart, in a parallel vector
 * indexed like the steps, and it is written ONLY ONCE: on the first frame where
 * the step stops being pending. It is a timestamp, not state - and this is
 * exactly the defect family this repo documents everywhere else (function-scope
 * `static` on a session path).
 */
#pragma once

#include "anim.h"

#include <nanovg.h>

#include <string>
#include <vector>

namespace ui {

enum class StepState { Waiting, Running, Done, Failed };

struct Step {
    std::string title;
    std::string detail;    /* trailing detail on the right: a code, a measure */
    StepState   state = StepState::Waiting;
};

class StepScreen {
public:
    void setTitle(std::string t)     { title_ = std::move(t); }
    void setSubtitle(std::string s) { subtitle_ = std::move(s); }

    /* Replaces every step. Called once, when the plan is built. */
    void setSteps(std::vector<Step> e) { steps_ = std::move(e); }

    /* Updates ONE step. An out-of-range index is silently ignored: the plan can
     * change between two versions, and background work aiming at a step that no
     * longer exists must not bring the display down. */
    void setStep(size_t i, StepState state, std::string detail = std::string());

    /* The bottom message: what happened, or what we are waiting for. `error`
     * colours it and makes it stand out - a failure must not read like one more
     * piece of information. */
    void setMessage(std::string m, bool error);

    /* Button hints, bottom right (see screen.hpp). */
    struct Hint { std::string button, label; };
    void setHints(std::vector<Hint> h) { hints_ = std::move(h); }

    void draw(NVGcontext *vg, float x, float y, float w, float h, double t);

private:
    std::vector<Step> steps_;
    std::string title_, subtitle_, message_;
    bool        message_error_ = false;
    std::vector<Hint> hints_;

    /* S80 - the instant each step stopped being pending. `< 0` = not yet.
     * Written ONCE, in `draw()` (the only place that knows the time), never in
     * `setEtapes()` - which is called every frame and would therefore reset
     * everything sixty times a second. */
    std::vector<double> appeared_;

    /* The filled portion of the ring, animated. It targets the real progress;
     * re-targeting the same value restarts nothing (anim.h), so calling it every
     * frame is free and makes the motion SELF-HEALING. */
    anim_t ring_anim_ = anim_fixed(0.0f);

    void drawRing(NVGcontext *vg, float cx, float cy, float r, double t);
};

}  // namespace ui
