/* SettingsView - the settings screen, drawn by the in-house framework.
 *
 * === WHY IT LEFT BOREALIS ===
 *
 * This one screen concentrated four defects, every one of them traceable to
 * its being built out of Borealis widgets assembled in XML:
 *
 *   - toggles showed a SENTENCE instead of their state, because `BooleanCell`
 *     writes "ON"/"OFF" into the same area as the detail line, and our
 *     descriptions overwrote it;
 *   - those "ON"/"OFF" stayed in English: the French translation was never
 *     applied, because the key of a Borealis catalogue is the FILE NAME and
 *     ours was misnamed;
 *   - text that was too long overflowed its row, with neither wrapping nor
 *     scrolling;
 *   - going back with B CRASHED (S63/S66).
 *
 * None of the four is an accident: they all come from controlling neither the
 * rendering, nor how labels are resolved, nor how long views live. Here all
 * three belong to us.
 *
 * === WHAT CHANGES FOR THE USER ===
 *
 * A setting is no longer a view: it is one entry of a `ui::ListScreen`.
 * Toggles have a DRAWN SWITCH, readable from across the room and impossible to
 * cover with text. Choices are cycled left/right with their value shown. The
 * section headings are non-focusable, so navigation skips them instead of
 * stopping on them.
 *
 * The screen holds NO pointer to a view and destroys none: changing a setting
 * rebuilds a vector of values. That is what closes the S63 family of crashes
 * for good.
 *
 * === S79 2026-08-29 - THE SECTIONS ===
 *
 * The page had become a SINGLE COLUMN of thirty-four entries under eleven
 * headings. It worked, but it could no longer be scanned: reaching "Langue"
 * took twenty-eight presses of down, and nothing on screen said how many were
 * left. Worse, two neighbouring headings have nothing to do with each other -
 * "Audio" and "Decodage" followed one another without that adjacency meaning
 * anything at all.
 *
 * So each heading becomes a SECTION, picked from a column on the left. The
 * list on the right shows only the open section: six entries instead of
 * thirty-four, and one glance tells you what the application knows how to set.
 *
 * `build()` is split per section and produces exactly one at a time. It is
 * that split, not the rail, that buys the readability: the section table and
 * the content come from the SAME place, so a section cannot exist in the
 * column without having any content.
 */
#pragma once

#include <borealis.hpp>

#include "../ui/screen_base.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../ui/screen.hpp"

class SettingsView : public ui::Screen {
public:
    /* DEVL-4: the name a script navigates by. Stable, never the title. */
    const char *devName() const override { return "reglages"; }

    SettingsView();

    /* Rebuilds the whole list from the current settings. Called on open and
     * after every change: this is what keeps the display and the state in
     * agreement, without having to patch one precise entry. */
    void rebuild();

    bool up()   override { return screen_.up(); }
    bool down()    override { return screen_.down(); }
    bool left() override;
    bool right() override;
    bool triggerL() override { return moveSection(-1); }
    bool triggerR() override { return moveSection(+1); }
    bool activate() override;                  /* A: toggles, or fires an action */
    bool touch(float x, float y);

    /* An action that leaves this screen (sub-page, logout). The activity
     * supplies it: the view knows of no other activity. */
    void setOnAction(std::function<void(int)> cb) { on_action_ = std::move(cb); }

    void paint(NVGcontext *vg, float x, float y, float w, float h,
                 double t) override;

    /* S97 - this screen's footer is tappable. */
    ui::ListScreen *innerList() override { return &screen_; }

private:
    ui::ListScreen screen_;
    std::function<void(int)> on_action_;

    /* === S86 - THE MAILBOX OF THE CLEAR-LOGS DIALOG ===
     *
     * The "Effacer" button lives inside a `brls::Dialog`, whose lifetime is not
     * ours. Making it capture `this` so it could call `rebuild()` back would
     * reintroduce exactly what this whole framework removes: a pointer to a
     * view, held by an object that may outlive it - the S63 family of crashes.
     *
     * So the dialog writes into a SHARED boolean instead. If the screen is gone
     * by then, the write lands in a boolean that is still alive and has no
     * reader: nothing to dereference, nothing to crash. The screen picks it up
     * AFTER drawing, the same way it already picks up `touchRelease()` and
     * `sectionConsumed()` - rebuilding the list while we are drawing it is the
     * reentrancy this framework avoids everywhere else. */
    std::shared_ptr<bool> purge_done_ = std::make_shared<bool>(false);

    /* Applies to the setting whatever the focused entry has just changed.
     * Returns true if something moved. */
    bool apply(const ui::Item &it);

    /* Moves the open section and rebuilds its content. Returns true only if it
     * actually changed - otherwise the trigger is left unconsumed and Borealis
     * disposes of it, which avoids swallowing a button press for nothing. */
    bool moveSection(int dir);

    /* Fills `v` with the content of section `r`, and nothing else. */
    void build(int r, std::vector<ui::Item> &v);

    /* Opens the confirmation for clearing the logs (S86). Handled here rather
     * than through `on_action_`: it does not leave the screen, and it has to
     * rebuild it afterwards - same as SET_DEV_CHANNEL_NOW. */
    void askClearLogs();
    void askReset();
    void askRemoveLock();
};

/* === SECTION identifiers ===
 *
 * Distinct from the entry identifiers (`SetId`), and deliberately so: the two
 * tables travel through the same screen, and a shared identifier would let a
 * section answer for a setting. Numbered from 100 so that the confusion shows
 * up immediately in a log. */
enum SetSection {
    SEC_PAGES = 100,   /* the sub-pages: video, gamepad, gestures */
    SEC_CONNECTION,
    SEC_SESSION,
    SEC_AUDIO,
    SEC_IMAGE,
    SEC_CONTROLS,
    SEC_INTERFACE,
    SEC_SECURITY,
    SEC_ADVANCED,
    SEC_ACCOUNT,
    SEC_ABOUT,   /* S85 - version, build date, git hash */
};

/* Entry identifiers. These are what travel, never an index: indices shift as
 * soon as a section appears or disappears (the bitrates are only shown when
 * "bitrate per link" is on). */
enum SetId {
    SET_VIDEO = 1, SET_GAMEPAD, SET_LOGOUT,
    SET_REPLAY, SET_REPLAY_AUTH,
    SET_POINTER,
    SET_AUTO_CONNECT, SET_VIDEO_TCP, SET_PAD_ENABLE, SET_REG_INPUT,
    SET_AUTO_RES, SET_RATE_LINK, SET_RATE_WIFI, SET_RATE_ETH,
    SET_KB_LAYOUT, SET_UI_SCALE, SET_LANGUAGE,
    /* 2026-08-27 - capabilities that existed with no setting, hence out of
     * reach on a console: what happens when focus is lost, reconnection, the
     * two hardware decoders, the dev channel. Plus three settings that lived
     * only in the pause menu, hence unreachable OUTSIDE a session - and the
     * volume is usually set BEFORE launching. */
    SET_FOCUS, SET_RECO, SET_RECO_MAX, SET_HW_VIDEO, SET_HW_AUDIO, SET_DEV_CHANNEL,
    SET_VOLUME, SET_TOUCH_SENS, SET_MOUSE_REL,
    SET_DEV_CHANNEL_NOW,   /* "connect now": see settings_view.cpp */
    SET_GESTURES,
    SET_AUDIO_HIFI,
    SET_LOG_LEVEL,       /* S81 - log level, "Avance" section */
    SET_SOUNDS,          /* S88 - interface sounds, "Interface" section */
    SET_SOUNDS_VOL,
    SET_HAPTICS,
    SET_EQ,              /* S90 - equalizer sub-page */
    SET_CURSOR,          /* S113 - which cursor to draw (see settings.hpp) */
    SET_CLIPBOARD,       /* CLIP4 - which way the clipboard travels */
    SET_NET_FAMILY,      /* NET1 - IPv4 / IPv6 / automatic */

    /* S86 2026-08-29 - the LOGS themselves, and no longer just their level.
     * Setting the verbosity without being able to see what is written, or to
     * know what it takes up, or to reset it, is a dial with no instrument. */
    SET_LOG_SIZE,        /* information row: space used, sessions kept */
    SET_LOG_VIEW,        /* opens the viewer screen */
    SET_LOG_CLEAR,       /* erases - with a confirmation, it is destructive */

    /* S85 - three information rows, never focusable. */
    SET_STRETCH,         /* 2026-09-02 - fill the screen instead of the ratio */
    SET_LOCK_STATE,      /* 2026-09-02 - information row: is the app locked */
    SET_LOCK_PIN, SET_LOCK_PASSWORD, SET_LOCK_PATTERN,
    SET_LOCK_WAKE, SET_LOCK_REMOVE,
    SET_ENV_OVER,        /* 2026-09-02 - which settings env.txt has taken over */
    SET_RESET,           /* 2026-09-02 - back to the original values */
    SET_A_VERSION, SET_A_DATE, SET_A_HASH,
    SET_A_TAG,
    SET_A_LICENSE,
    SET_A_SOURCE,
    SET_A_AUTHOR,
    SET_A_TIME_DAY, SET_A_TIME_MONTH, SET_A_TIME_TOTAL,
};
