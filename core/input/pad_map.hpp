/* padmap - mapping between the console's buttons and the Shadow
 * envoyee a Shadow.
 *
 * On the VM's side, Shadow sees a DualShock: its identifiers are a
 * manette PlayStation (Carre, Triangle, Croix, Cercle, L1/R1, L3/R3, Start,
 * Select, Guide), and its L2/R2 triggers are analog AXES, not buttons. A
 * Switch's buttons therefore do not map onto them by themselves: a table is
 * needed, and the user must be able to change it - personal layouts, third-party
 * controllers, games that expect a specific button.
 *
 * The default table honours the physical POSITION rather than the letter:
 * X (haut) -> Triangle, B (bas) -> Croix, Y (gauche) -> Carre, A (droite) ->
 * Circle. Mapping the letters A->A would give buttons that read inverted on
 * screen compared with what the finger expects.
 */
#pragma once

#include <cstdint>
#include <string>

namespace padmap {

/* The console's buttons, in the settings menu's display order. */
enum class Btn {
    A, B, X, Y,
    L, R, ZL, ZR,
    Minus, Plus,
    LStick, RStick,
    Up, Down, Left, Right,
    COUNT
};

/* === S83 2026-08-29 - THE TARGET'S NAME FOLLOWS THE ANNOUNCED CONTROLLER ===
 *
 * The protocol's identifiers are those of a PlayStation controller, and this
 * screen displayed them as such: "Carre", "Triangle", "Croix", "Cercle",
 * "L1/R1", "Select". That was right as long as Shadow could only announce a
 * DualShock. Since G56 the announced type is chosen - and the default is an
 * **Xbox 360**, because XInput is the stack best supported by Windows games.
 *
 * Consequence: the screen showed PlayStation names for a controller Windows
 * presents as an Xbox. Setting "A -> Circle" then means nothing verifiable: the
 * game will display "B". You CANNOT check your own mapping by reading the
 * screen, which is nonetheless its only purpose.
 *
 * The NUMBER does not change - that is the wire, and it is fixed. Only the NAME
 * changes, to designate the same physical button in the announced controller's
 * vocabulary. The table is therefore indexed by family AND by position, never by
 * letter: on a Nintendo, the BOTTOM button is B and the RIGHT one is A, the
 * opposite of an Xbox. Mapping the letters would give buttons crossed over
 * relative to what the finger expects. */
enum class Family {
    Xbox = 0,        /* Xbox 360 and Xbox One: the same vocabulary */
    Playstation,     /* DualShock 4 */
    Nintendo,        /* ControllerLeft / ControllerRight */
    Generic          /* Default: we name the POSITIONS */
};

/* The family matching the controller type currently announced to the VM
 * (`Settings::gamepad_type`). An out-of-range value returns `Xbox`, the
 * setting's default - not `Generic`: showing "Bottom / Right" for a corrupted
 * value would be less useful than showing the default. */
Family     currentFamily();
/* The family's name, NOT localised -- "" means the caller must translate.
 * See the note on `targetSigle` below for why the split. */
const char *familyNameRaw(Family f);

/* What a button triggers on Shadow's side. Values 0..10 are the protocol's
 * identifiers (ctrl_gamepad.h); beyond that, targets that are not buttons. */
enum Target {
    TARGET_NONE  = -1,
    /* 0..10 = Shadow's own identifiers (SHADOW_PAD_*) */
    TARGET_L2    = 11,   /* analog trigger: 255 pressed, 0 released */
    TARGET_R2    = 12,
    TARGET_DPAD  = 13,   /* feeds the d-pad mask */
    TARGET_COUNT = 14
};

size_t      count();
const char *key(Btn b);            /* i18n key for the console button's name */
int         target(Btn b);         /* the current target */
void        setTarget(Btn b, int target);
/* Cycles a button's target (left/right in the menu). */
void        cycleTarget(Btn b, int dir);
/* A target's name in the requested family's vocabulary, NOT localised.
 *
 * Most of these names are initialisms identical in every language ("RB", "L1",
 * "ZR"), and giving each one a catalogue entry per language AND per family would
 * mean fifty-six keys to maintain for about ten genuinely translatable words. So
 * this returns the initialism directly, and "" for the ten that must be
 * translated - "Menu", "View", "D-pad", "None" - whose key `targetGenericKey`
 * gives.
 *
 * IT USED TO TRANSLATE HERE, and that was the last thing in `core/` reaching up
 * into the Borealis client (`ui/i18n.hpp`). The vocabulary is data and belongs
 * to the core; turning it into a sentence on screen belongs to the client, which
 * does it in `clients/borealis/ui/pad_label.hpp`. */
const char *targetSigle(int target, Family f);
/* The catalogue key for a target, for the names that ARE translated. Never
 * null: an out-of-range target gives "pad/unmapped". */
const char *targetGenericKey(int target);

void resetDefaults();
/* Reads and writes the table into Settings (a compact "3,2,1,..." string). */
void load();
void save();

/* Stick settings. */
bool     invertY();
void     setInvertY(bool v);
uint32_t deadzone();              /* en pourcentage, 0..40 */
void     setDeadzone(uint32_t v);

}  // namespace padmap
