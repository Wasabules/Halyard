# Porting the "machine list" screen — the reference specification

**Purpose**: describe *exactly* what the screen does today, so that it can be
rewritten on the in-house framework (focus = an integer index, screens = data,
redrawn every frame) **without losing a single behaviour** and **without
copying its defects**. This document is meant to be sufficient: reopening the
old code should not be necessary.

**Sources read** (everything asserted below cites `file:line`):

| File | Role |
|---|---|
| `clients/borealis/activity/vm_list_activity.cpp` (449 l.) | all the logic |
| `clients/borealis/include/activity/vm_list_activity.hpp` (45 l.) | the XML bindings + 2 state flags |
| `resources/xml/activity/vm_list.xml` (99 l.) | the view tree and its metrics |
| `resources/i18n/fr/shadow.json` §`vm` (l. 273-297), §`action` (l. 311-320), §`quit` (l. 78-82) | the texts |
| `core/services/launcher.{h,c}` | the 3 network calls and the shape of their replies |
| `clients/borealis/include/activity/shadow_app.hpp` | the singleton holding the cross-screen state |
| `third_party/borealis/library/lib/core/{thread,application,activity,view}.cpp` | the exact semantics of what the screen delegates to Borealis |
| `/tmp/halyard/stderr.log:3` | **field evidence** of what `/vms` really returns on the test account |

---

## 0. Where it sits in the application

The activity stack: `BootActivity` → **`VmListActivity`** → (`SettingsActivity`
| `ConnectingActivity` → `StreamActivity`).

- Pushed exactly once, from `boot_activity.cpp:302`. **Never popped**: the B
  button is hijacked (`vm_list_activity.cpp:133-145`) into an application-exit
  request, so we never drop back to the boot screen.
- Exits: `pushActivity(SettingsActivity)` (`:125`) and
  `pushActivity(ConnectingActivity)` (`:447`).
- Returning from a stream: `StreamActivity` is popped → `ConnectingActivity::onResume`
  (`connecting_activity.cpp:145-150`) → that screen pops itself when the exit
  was deliberate (`exit_reason == 3`, S40, `connecting_activity.cpp:759-776`) →
  **`VmListActivity::onResume` is called once**, not twice.
- Returning from Settings: B → `popActivity` → one `onResume` as well.

> **To remember when porting**: this screen is the application's *practical
> root*. It is the one place the user always comes back to; its robustness
> conditions everything else.

---

## 1. The current visual anatomy (to reproduce, or to diverge from deliberately)

The view tree from `vm_list.xml`, with all its metrics, because an
"approximately right" port shows up immediately on a 6-inch screen.

```
AppletFrame                       title = @i18n/shadow/vm/title = "My Shadow machines"
│                                 header visible, footer visible (xml:3-7)
└── Box column, grow=1            left/right margins = @style/brls/applet_frame/padding_sides
    │                             = 30 px (style.cpp:53)                      (xml:12-18)
    ├── Header  "Data centre"                                                 (xml:20-22)
    ├── Label   #dc_label         fontSize 20, marginTop 6, marginBottom 8,
    │                             colour @theme/brls/text_disabled            (xml:24-30)
    ├── Box row, alignItems=center, marginBottom 20                           (xml:33-38)
    │   ├── Label #plan_label     fontSize 16, text_disabled                  (xml:39-43)
    │   └── Label #drive_label    fontSize 14, marginLeft 14, text_disabled   (xml:44-49)
    ├── Header  "Virtual machines"                                            (xml:52-54)
    ├── ScrollingFrame #vm_scroll grow=1, marginTop 10                        (xml:57-62)
    │   └── Box #vm_container     column, paddingTop 10, paddingBottom 20     (xml:64-70)
    │       └── (cards injected at runtime)
    └── Box row, alignItems=center, marginTop 10, marginBottom 10             (xml:75-81)
        ├── ProgressSpinner #loading  24×24, visibility=invisible             (xml:83-87)
        └── Label #status             fontSize 18, marginLeft 12, text_disabled (xml:89-94)
```

A machine card — `make_vm_card()`, `vm_list_activity.cpp:28-81`:

| Property | Value | Line |
|---|---|---|
| container | `Box` column, `focusable(true)` | `:32,42` |
| height | **a fixed 96 px** | `:33` |
| margins | top 6, bottom 6 | `:34-35` |
| padding | left/right 20, top/bottom 14 | `:36-39` |
| corner radius | 8 px | `:40` |
| background | `nvgRGB(35, 40, 55)` — **hardcoded, it does not follow the theme** | `:41` |
| title | the alias, `fontSize 28`, `nvgRGB(240,240,245)` | `:45-49` |
| state | `fontSize 16`, `marginTop 4`, colour from the classifier | `:52-71` |
| activation | `registerClickAction` (= the A button) + a `TapGestureRecognizer` | `:74-78` |

State colours (`:59-69`) — **read §8, this classifier never fires**:

| Condition (substring, case-sensitive) | Colour |
|---|---|
| default | `nvgRGB(140,160,180)` blue-grey |
| contains `ACTIVE` or `READY` | `nvgRGB(80,200,120)` green |
| contains `STARTING` or `BOOTING` | `nvgRGB(240,180,80)` amber |
| contains `ERROR` or `FAIL` | `nvgRGB(220,90,90)` red |

The "empty list" message (`:331-337`): `fontSize 20`, centred, `marginTop 40`,
`nvgRGB(160,160,170)`.

**The focus highlight**: entirely delegated to Borealis (an animated blue frame
around the focused view). The in-house framework must provide it itself — it is
the only visual cue of selection; there is no other marking.

**Theme**: on the Switch the theme follows the console setting
(`switch_platform.cpp:79-81`). In the dark theme, `brls/text_disabled` is
`nvgRGB(80,80,80)` over an `nvgRGB(45,45,45)` background (`theme.cpp:87,89`):
the four ambient labels (`dc`, `plan`, `drive`, `status`) are then close to
unreadable. See §8 (D9).

---

## 2. The data displayed — where each text comes from

### 2.1 Provenance table

| Element | Text displayed | Where it comes from | When |
|---|---|---|---|
| Page title | `vm/title` = "My Shadow machines" | static, from the XML | at construction |
| `dc_label` | `ShadowApp::dc_name`, else `vm/dc_unknown` = "unknown" | TINAG (`boot_activity.cpp:130`, the `gi.name` field) — **no network call here** | `onContentAvailable`, `:102`, **exactly once** |
| `plan_label` | `vm/plan_loading`, then a composed text (§2.3) | `launcher_get_subscription_status()` — `GET {API_V1}subscription/status?product_family=cloudpc` (`launcher.c:850-890`) | on the background thread, **exactly once** (`:106`) |
| `drive_label` | `vm/drive_on` / `Drive: <message>` / `vm/drive_off` | `launcher_get_drive_token()` — `GET {API_V1}pu/shadow-drive/user/token?token_name=shadow-web-launcher-token&force=1` (`launcher.c:902-932`) | the same thread, right after |
| (Switch) `drive_label` suffix | `"  ⚠ Battery N% (plugging in recommended)"` when `< 15 %` | `psmGetBatteryChargePercentage` (`:222-231`) | the same thread |
| Card title | `alias`, else `name` | `launcher_list_vms()` — `GET {launcher_url}vms?offset=0&limit=50` (`launcher.c:173-234`) | on every refresh |
| Card state | the server's raw `state`, else `vm/state_unknown` = "state unknown" | the same call | ditto |
| `status` (footer) | `vm/loading` → `vm/count` or `vm/http_error` | composed locally | ditto |
| **Badges** | *none* | — | — |

> There is **no badge** anywhere in the current screen: no plan, no per-machine
> data centre, no maintenance flag, no tags. The only conditional decoration is
> the colour of the state text. If the port adds any, that is a new feature,
> not parity.

### 2.2 The exact order of the network calls

`onContentAvailable()` posts, **in this order**:

1. `fetchAccountInfo()` (`:106`) → one background task that chains the
   **subscription** then the **Drive** call (`:158`, `:162`);
2. `refreshVms()` (`:147`) → one background task that calls **the VM list**.

Both tasks go onto Borealis's **single background thread**
(`thread.cpp:191-208`), which runs them **in series**. The measurable
consequence: **the machine list only appears once both account requests have
finished**, requests to `api.eu.shadow.tech` that have nothing to do with it.
See §8 (D4).

### 2.3 Composing `plan_label` (`:167-196`)

On success (`ok_sub && sub.plan_short`) — concatenated, in this order:

1. `vm/plan` → "Plan: {plan_short}". `plan_short` is **derived** from
   `plan_id` by `derive_plan_short()` (`launcher.c:824-848`):
   `cloudpc-b2c-power2023-EUR-Monthly` → `Power 2023`. Recognised families:
   `power`, `boost`, `ultra`, `starter`; otherwise the raw `plan_id`; `NULL` → `"Unknown"`.
2. `" (" + status + ")"` **only if** `status != "active"` (`:168-169`).
3. `" - " + vm/plan_hold` ("on hold") if `sub.on_hold` (`:170`).
4. `" - " + vm/plan_since` ("active since DD/MM/YYYY") if `started_at > 0`, the
   date formatted in **local time** (`:172-183`).
5. `" - " + vm/payment_issue` ("payment: X") **only if** `last_payment_status`
   is in none of the healthy values: `ok`, `succeeded`, `succeed`,
   `payment_succeed`, `payment_succeeded`, `success`, `paid`, empty (`:190-196`).
   The original comment says why the list is so long: Shadow returns
   `payment_succeed` when everything is fine, and a perfectly healthy
   subscription was being displayed as an anomaly.

On failure — a diagnosis **per HTTP code** (`:199-202`):

| `st_sub` | Text |
|---|---|
| `0` | `vm/plan_offline` — "Plan: network unreachable" |
| `401` | `vm/plan_expired` — "Plan: session expired, you need to sign in again" |
| `>= 500` | `vm/plan_server` — "Plan: server unavailable (HTTP {})" |
| anything else | `vm/plan_error` — "Plan: HTTP error {}" |

### 2.4 Composing `drive_label` (`:205-208`)

- `ok_drv && drv.token` → `vm/drive_on` = "Drive: enabled"
- `ok_drv && drv.message` → `"Drive: " + message` (**a hardcoded, untranslated string**)
- otherwise → `vm/drive_off` = "Drive: unavailable"

### 2.5 The real shape of `/vms` — **measured, not assumed**

`launcher_list_vms` accepts several envelopes (`launcher.c:199-208`): an array
at the root, or an `entries` / `vms` / `items` / `data` key. The total comes
from `pagination.total_count`, else `total`, else the number of elements
(`launcher.c:219-228`). The call is made with `offset=0, limit=50` (`:291`) and
**there is no pagination on the UI side at all**.

`parse_vm` (`launcher.c:126-171`) reads `id`, `alias`, `name` (falling back to
`hwconfig`), then looks for the state in `state`, else `status` (a string),
else `status.state`.

**What the server actually returns**, logged once by `launcher.c:153-166` and
found in `/tmp/halyard/stderr.log:3`:

```
launcher: no VM state; keys seen: id, name, hwconfig, datacenter,
          maintenance, tags, provider, status, siberia_disabled,
          graphic_driver_reinstall
```

Three consequences, all verified:

- **there is no `alias` key** → the card title is always `name`
  (or `hwconfig` when `name` is empty);
- **`state` does not exist, and `status` is neither a string nor an object
  carrying `state`** → `sv.state` is **always empty** → every card displays
  "state unknown" in grey;
- the server exposes `datacenter`, `maintenance`, `tags` and `provider` per
  machine — **four available fields the screen does not display**.

### 2.6 Conversion into the singleton (`:304-313`)

`ShadowApp::vms` (`shadow_app.hpp:45`) is **cleared then refilled on the
background thread**. `ShadowVm` = `{id, name, alias, state, image_url}`
(`shadow_app.hpp:28-34`); `image_url` is never filled in. `alias` takes
`v->alias`, else `v->name`, else `""` (`:310`). `page.total` is copied into a
local before `vmpage_free` (`:314-315`).

No other screen reads `ShadowApp::vms` (an exhaustive search over `demo/`: only
`ShadowApp::logout()` clears it, `shadow_app.hpp:79`): that vector is nothing
but an internal buffer for this screen. The port can replace it with a local
array without breaking anything elsewhere — **but** `selected_vm_id` and
`selected_vm_alias` (`shadow_app.hpp:48-49`) *are* read by `ConnectingActivity`
and must stay.

---

## 3. Threading — the most important point of the port

### 3.1 What Borealis provides, exactly

| Call | Real semantics | Reference |
|---|---|---|
| `Threading::async(f)` | pushes `f` onto a queue drained by **a single thread**, woken every **500 ms** (`retro_sleep(500)`) | `thread.cpp:73-77`, `191-208` |
| `Threading::sync(f)` | pushes `f`; run on the **main thread**, after `frame()`, on the next loop iteration | `thread.cpp:67-71`, `application.cpp:201-204` |
| `Threading::delay(ms, f)` | **creates no thread**; `f` runs on the main thread from `performSyncTasks` as soon as the delay has elapsed; returns an id that `cancelDelay` can cancel | `thread.cpp:79-95`, `121-167` |

Two properties the screen depends on without saying so:

1. **Serialisation**: two `async`s posted back to back never overlap. That is
   what currently stops two `refreshVms()` from treading on each other — by
   accident, not by design (§8, D5).
2. **The order within one main-loop iteration**: `frame()` (`application.cpp:201`)
   → `performSyncTasks()` (`:204`) → **draining the deletion pool**
   (`:210-224`). That is the only reason S63 works: a view removed by
   `clearViews()` during a `sync` task is only destroyed **after** that task
   finishes.

### 3.2 The `alive` flag — the complete protocol

- Type: `std::shared_ptr<std::atomic<bool>>` (`vm_list_activity.hpp:40`).
- Created as `true` in the constructor (`:93`), set to `false` in the
  destructor (`:97`).
- **Copied by value** into every background lambda: it is the `shared_ptr` that
  guarantees the flag outlives the activity, not the activity that guarantees
  the flag.
- `ui_run(alive, fn)` (`:84-90`) checks **twice**: before posting, and inside
  the `sync` task. **Only the second check counts** — it runs on the main
  thread, so it cannot race with the destructor, which also runs on the main
  thread.
- An extra check in the middle of the background work, just after the network
  call (`:293`), to avoid consuming a reply nobody wants.
- The deferred auto-connect re-checks `alive` before acting (`:391`).

**This is the only mechanism that makes the current code safe, and it is
correct.** The in-house framework must provide the equivalent, preferably *not
optional*: every piece of background work must be tied to a validity token, and
a task whose token is dead must never reach the UI thread.

### 3.3 The timeline of the first display

```
t0     onContentAvailable: dc_label, plan_label="Plan: loading…", drive_label=""
       async #1 (account) posted; 4 actions registered; refreshVms()
       sync #1 posted: status="Loading…", spinner visible, clearViews, focus→vm_scroll
       async #2 (list) posted
t0+≤16 ms  main thread: sync #1 runs
t0+≤500 ms background thread: wakes up, runs async #1
       … GET subscription/status … GET shadow-drive/user/token …
       sync #2 posted: plan_label, drive_label
       then async #2: GET vms?offset=0&limit=50
       sync #3 posted: renderVmCards(), status="N machine(s) of M", spinner hidden
```

So at worst the list waits `500 ms + 2 HTTP round trips` before its own request
even goes out.

---

## 4. Actions — every button

| Button | Footer label | Registered where | Exact effect |
|---|---|---|---|
| **A** | `hints/ok` = "OK" | `registerClickAction` on **each card** (`:74-77`), through `View::registerClickAction` → `BUTTON_A` (`view.cpp:768-771`) | `onVmClicked(id, alias)` |
| **B** | `action/quit` = "Quit" | the activity (`:133-145`) | opens a `brls::Dialog` `vm/quit_question` = "Quit the application?", cancellable (B closes it), two buttons: `quit/stay` then `action/quit` → `Application::quit()` |
| **X** | `action/refresh` = "Refresh" | the activity (`:109-115`) | `refreshVms()` |
| **Y** | `settings/open` = "Settings" | the activity (`:121-127`) | `pushActivity(new SettingsActivity())` |
| **+** | `hints/exit` = "Quit" | **Borealis**, automatically on every `pushActivity`, because `main.cpp:325` calls `setGlobalQuit(true)` (`application.cpp:917-918`, default `BUTTON_START`, `activity.hpp:154`) | `Application::quit()` **with no confirmation** |
| **−** | — | nothing | nothing |
| Touch | — | the card's `TapGestureRecognizer` (`:78`) | gives the card focus **then** fires its A action (`tap_gesture.cpp:22-56`); the `ScrollingFrame` separately handles finger scrolling |

Points not to miss:

- `View::registerAction` **replaces** an action already registered on the same
  button (`view.cpp:751-752`). The screen's B therefore overrides the
  `AppletFrame`'s default B, which is "go back" (`applet_frame.cpp:119-124`).
  That is deliberate: this screen must not drop back to the boot screen.
- `Activity::registerAction` delegates to the root view (`activity.cpp:120-126`):
  the actions live on the `AppletFrame`, an ancestor of the focused card, and so
  are visible whichever card is focused.
- **Footer display order** (`hint.cpp:150-215`): `+` first, then the "other"
  buttons in registration order (X, Y), then `B`, then `A`. One action per
  button, and the one closest to the focus wins.
- So the footer shows **the word "Quit" twice** (`+` and `B`) for two different
  gestures. See §8 (D8).

`onVmClicked(id, alias)` (`:430-449`):

1. returns without doing anything if `vm_id` is empty (`:432`);
2. writes `ShadowApp::selected_vm_id` and `selected_vm_alias` (`:435-436`);
3. `app.resetAbort()` (`:437`) — **essential**: it clears the shared abort flag
   that the stream session will watch (`shadow_app.hpp:84-86`);
4. logs three `[S6]` milestones (`:443`, `:446`, `:448`) that bracket the
   construction and the push of `ConnectingActivity` — they exist because the
   console used to crash at exactly that transition. **Keep them** until the
   port has run several sessions without incident.

---

## 5. The screen's states

| State | Trigger | `status` | `vm_container` | `spinner` | Focus |
|---|---|---|---|---|---|
| **Initial** | XML loaded | `""` | empty | invisible | Borealis's default |
| **Loading** | `refreshVms` (`:277-282`) | `vm/loading` = "Loading…" | **cleared** | visible | `vm_scroll` |
| **Populated** | success (`:318-322`) | `vm/count` = "N machine(s) of M" | N cards | invisible | **the first card** (`:359`) |
| **Empty** | success, 0 machines (`:330-346`) | "0 machine(s) of 0" | 1 centred label `vm/empty` = "No machine on this account." | invisible | `vm_scroll` (`:344`) |
| **HTTP error** | `!ok` (`:295-301`) | `vm/http_error` = "HTTP error {}" | **stays empty** | invisible | `vm_scroll` (set during the Loading step) |
| **Account unreachable** | `fetchAccountInfo` failed | unchanged | unchanged | unchanged | unchanged; only `plan_label`/`drive_label` carry the diagnosis (§2.3) |

**A machine that is off against one that is on**: *no visual distinction
actually exists*. The colour classifier (`:59-69`) is written for
`ACTIVE`/`READY`/`STARTING`/`BOOTING`/`ERROR`/`FAIL` in upper case; the server
returns no usable state (§2.5). In practice **every card is identical**, showing
"state unknown" in blue-grey. The A button behaves the same either way:
`ConnectingActivity` starts the machine if needed (`launcher_start_vm`). The
screen forbids nothing, greys out nothing and signals nothing.

---

## 6. Auto-connect (S62)

### 6.1 The exact rule, as implemented (`:361-394`)

```
auto_env = getenv("SHADOW_AUTO_CONNECT")
auto_connect = auto_env ? (atoi(auto_env) != 0) : Settings::instance().auto_connect
if auto_connect AND !auto_connect_done AND app.vms.size() == 1:
    auto_connect_done = true
    Threading::delay(500, → onVmClicked(vms[0].id, alias))
```

- The `Settings::auto_connect` setting is **`true` by default**
  (`settings.hpp:154`), is persisted in `settings.txt`
  (`settings.cpp:46-47`, `:120`) and is set from the Settings screen
  (`settings_activity.cpp:94-95`, labels `settings/auto_connect`).
- The environment variable **wins** when it is defined — that is for test
  automation, not for the user (it is unreachable on console).
- The "exactly one machine" condition: with 0 or ≥ 2, nothing fires.

### 6.2 When it arms, and when it must **not**

`auto_connect_done` is a member of the activity
(`vm_list_activity.hpp:27`), so:

- **it arms** on the **first** card construction after the application starts;
- **it never arms again** on any later render — in particular not on the return
  from a stream, even though `onResume` re-runs `refreshVms()` (S50) and
  therefore goes through `renderVmCards()` again.

**Why that matters** — the original comment (`:364-371`): without that latch,
leaving the stream started a new session 500 ms later, "with no way to reach the
settings or change machine without being sucked back in". That is the navigation
bug that motivated S62.

**Why `first_appearance` cannot serve as the latch** — the comment at
`vm_list_activity.hpp:23-26`: `refreshVms()` is asynchronous and builds its
cards **after** `onResume` has already set `first_appearance` to false; a latch
based on that flag would never arm the auto-connect. The two flags are separate
for good reasons — **do not merge them**.

### 6.3 The 500 ms delay

`Threading::delay` and **not** a detached thread: see §7, the S8 trap.

---

## 7. Known traps — with the reason each exists

### S63 (2026-08-27) — clear **first**, give focus **afterwards**

The full comment: `vm_list_activity.cpp:252-276`, duplicated for the "no
machine" case at `:338-343`.

The mechanics of the crash, as documented on the spot:

1. `Application::giveFocus(v)` resolves its target through `v->getDefaultFocus()`
   (`application.cpp:784`);
2. `Box::getDefaultFocus()` descends through `lastFocusedView`
   (`box.cpp:357-361`): giving focus to the container **returned the card that
   was already focused**;
3. `oldFocus == newFocus` → the guard at `application.cpp:807` skips the whole
   body → **a complete no-op**;
4. `clearViews()` then freed that card, while `Application::currentFocus` stayed
   pointing at it;
5. on the next frame, `vm_scroll` — **the application's only `NATURAL`
   `ScrollingFrame`**, the three settings screens being `centered` — called
   `naturalScrollingBehaviour()` → `giveFocus(this)` → `oldFocus->onFocusLost()`,
   a **virtual call on a freed vtable** → "Instruction Abort".

The fix: clear **then** give focus to the `ScrollingFrame`, which is focusable
from its constructor onwards (`scrolling_frame.cpp:40`) and, once its content is
empty, returns itself as the default focus. Card destruction is deferred to the
end of the iteration (`application.cpp:210-224`), so the old card's
`onFocusLost()` still runs on a valid object.

> **Borealis's own safety net does not work**: `View::~View` calls
> `giveFocus(nullptr)` (`view.cpp:1485-1487`), which was also a no-op. A
> **local** fix was applied in the vendored library
> (`application.cpp:786-805`, the block "LOCAL FIX halyard 2026-08-27") so
> that `giveFocus(nullptr)` really does reset `currentFocus` to `nullptr`.
> **The port must not depend on that fix**: it is a patch on upstream code and
> will disappear at the first Borealis update.

**In the in-house framework this trap no longer exists by construction** — the
focus is an integer. One obligation remains: **clamp the index when the list
shrinks** (keep the equivalent item if possible, otherwise go back to 0; never
leave an index ≥ size).

### S50 (2026-08-26) — reload the state when coming back from a stream

The comment: `:399-408`. `onResume` used to do nothing but restore focus; ever
since leaving the stream brings you back here (S40), the screen showed the state
from before the connection, "with no way out other than restarting the
application". Hence the `refreshVms()` in `onResume`.

**The guard condition is wrong** — see §8 (D1): `onResume` is never called on
the first appearance.

### S8 (2026-08-22) — no detached thread for a simple delay

The comment: `:381-389`. The old auto-connect launched a detached `std::thread`
that slept 500 ms. On the Switch, the console crashed systematically ~500 ms
after the list appeared, **before even** entering `onVmClicked`. And a detached
thread capturing `this` outlives the activity's destruction: a use-after-free
waiting to happen on **every** platform.

**The general rule to carry over**: never a raw thread for a delay, never a
capture of `this` without a validity token. The in-house framework must offer a
**cancellable** "run in N ms on the UI thread".

### The `refreshVms` ↔ `onResume` ordering

The real sequence when coming back from Settings or from a stream:

```
popActivity (application.cpp:824)
  ├─ :845  VmList::onResume()
  │        ├─ refreshVms()  → posts sync #1 (clear + focus→vm_scroll)  ← will run on the next frame
  │        └─ giveFocus(vmContainer->getChildren()[0])                  ← an OLD card
  └─ :848-864  focus restored from the focus stack:
               giveFocus(focusStack.back()) — the view focused at push time,
               which is, again, an OLD card (pushed at :901)
next frame:
  frame() → performSyncTasks(): sync #1 empties the container then focuses vm_scroll
          → drain the deletion pool: the old cards die here
```

Two lessons:

- `onResume`'s focus block (`:417-427`) is **immediately overwritten** by
  `popActivity`'s restoration: it is dead code in the nominal case;
- the screen only gets away with it because S63 moves the focus **before** the
  drain. Any rewrite that swapped those two moments would crash again.

---

## 8. Defects **not** to reproduce

**D1 — `first_appearance` eats the first return from a stream.**
`onResume()` is called **only** from `popActivity` (`application.cpp:845`, the
sole call site in the whole library), **never** on the first push. Yet
`first_appearance` starts out `true` (`vm_list_activity.hpp:21`), so the first
`onResume` — that is, the **first** return from a stream or from Settings —
takes the "do not reload" branch (`:409-412`). *S50 is only fixed from the
second return onwards.* The comment (`:406-408`) claims "the first appearance is
already preceded by a `refreshVms()` in the constructor", which is true, but it
wrongly concludes that an `onResume` corresponds to that first appearance.
**Port: reload on every return, no exception, and drop the flag.**

**D2 — the state classifier is dead code.** Six branches, four colours, zero
firings: the server sends no usable state
(`/tmp/halyard/stderr.log:3`), and the classifier tests upper-case
strings while `launcher.h:12` advertises `"running"`/`"stopped"`. **Port: do
not copy those branches.** Either identify what `status` really contains first
(the log at `launcher.c:153-166` exists for that), or display no state at all —
no dot beats a dot that is always grey.

**D3 — a failed refresh destroys the list that was working.** The container is
cleared **before** the request (`:280`); on failure we return at `:300` without
rebuilding anything. So a one-second Wi-Fi drop turns a correct list into a
blank page, whose only clue is "HTTP error 0" in 18 px at the bottom. **Port:
only clear at the moment of writing the new result, and keep the old one on
failure.**

**D4 — the list waits on two requests that have nothing to do with it.** Both
`async`s share a single thread (`thread.cpp:191-208`) and `fetchAccountInfo` is
posted first (`:106` before `:147`). The screen's main content is therefore
hostage to `subscription/status` and `shadow-drive/user/token`. **Port: start
the list first, or in parallel; the screen must be usable before the plan line
is known.**

**D5 — a data race on `ShadowApp::vms`.** The background thread writes the
vector (`:304-313`) while the main thread reads it (`:329-356`, `:375-378`).
Nothing separates them: if the user hits X twice in a row, the first
`renderVmCards` can iterate a vector the second is in the middle of clearing.
It is not observed today **by accident** — because Borealis serialises
background tasks. **Port: the background thread must produce only an immutable
value, handed to the UI thread; it must never write the shared state.**

**D6 — the deferred auto-connect is never cancelled.** `Threading::delay`
returns a cancellable id (`thread.cpp:79-95`); the return value is thrown away
(`:390`). During those 500 ms the user can open Settings (Y) or the exit dialog
(B): the connection will go ahead anyway and push `ConnectingActivity` **on
top**. It is the same family of bug as S62, in a narrower window. **Port:
remember the deferred call and cancel it on the user's first action.**

**D7 — three texts escape internationalisation.**
`"(unnamed VM)"` (`:46`), `"Drive: " + message` (`:207`), and
`"  ⚠ Battery N% (plugging in recommended)"` (`:229`) — hardcoded French, with
accents, in a file where everything else goes through `ui::tr`. The last one
also contains `⚠`, exactly the kind of glyph the comment at `:185-189` says the
font cannot render. **Port: three new keys in `resources/i18n/*/shadow.json`.**

**D8 — the footer shows "Quit" twice.** `+` quits with no confirmation
(`application.cpp:917-918`), `B` opens a confirmation (`:133-145`). Two
identical labels, two different behaviours. On top of that the dialog's cancel
button reuses `quit/stay` = "**Keep streaming**" (`:139`) when no stream is
running. **Port: label them distinctly, and give the dialog its own cancel
text.**

**D9 — unreadable contrast in the dark theme.** The four ambient labels use
`@theme/brls/text_disabled` (`xml:30,43,49,94`), which is `nvgRGB(80,80,80)`
over an `nvgRGB(45,45,45)` background in the dark theme
(`theme.cpp:87,89`), and that theme follows the console setting
(`switch_platform.cpp:79-81`). Meanwhile the card background is hardcoded
(`:41`) and follows nothing. **Port: a single explicit palette, independent of
the console theme — which is what `clients/borealis/ui/theme.hpp` already does for the
pause menu and the metrics panel.**

**D10 — information that is available and not displayed.** The server returns
`datacenter`, `provider`, `maintenance`, `tags` and `hwconfig` per machine
(`stderr.log:3`); the screen shows a **global** `dc_name` from TINAG and nothing
else. `raw_json` is kept by `parse_vm` (`launcher.c:169-170`) then thrown away
during the conversion (`:305-313`). **Port: at minimum, carry those fields up
into the screen model, even if displaying them comes later.**

**D11 — `limit=50` with no pagination.** `:291` asks for 50 entries, `vm/count`
displays "N machine(s) of M" where M can exceed N (`launcher.c:219-228`), and
nothing lets you see the rest. Rare, but silent.

**D12 — a stale i18n key.** `vm/refresh_hint` = "A to refresh"
(`shadow.json:277`): the refresh has been on **X** since `:109-115`, and that
key is no longer referenced anywhere. **Do not carry it over.**

**D13 — `fetchAccountInfo` is never called again.** Only in
`onContentAvailable` (`:106`). A plan that expires, a payment that goes through,
a Drive enabled mid-session: the line stays frozen until the application
restarts. Conversely the battery *would* deserve a refresh — it is measured
once, at launch (`:222-231`).

**D14 — the battery reading is done on the network thread.** `psmInitialize` /
`psmGetBatteryChargePercentage` / `psmExit` are called inside the background
task that does the HTTP requests (`:211-232`), and the result is concatenated
onto a label that talks about Shadow Drive. Two unrelated subjects in one label.

---

## 9. The minimum contract expected of the in-house framework

Deduced from everything above; each point repairs a real incident.

1. **Focus = an integer index**, clamped every frame against the list's current
   size; never a view pointer kept between two frames.
2. **A mandatory validity token** for any background work, checked on the UI
   thread before a result is applied (§3.2).
3. **A cancellable "run in N ms on the UI thread"** — with no thread created
   (S8, D6).
4. **The background thread publishes only immutable values**; it writes no
   shared state (D5).
5. **The screen model = a data structure**, redrawn in full:
   `{ dc, plan, drive, machines[], focus_index, phase, message }`. `phase` ∈
   { initial, loading, populated, empty, error }.
6. **A failed refresh leaves the previous model intact** (D3).
7. **An explicit palette**, extending `ui::theme` (`clients/borealis/ui/theme.hpp`),
   independent of the console theme (D9).
8. **A pure module, testable offline**: composing the labels (§2.3, §2.4),
   classifying the state and clamping the focus index are pure functions — they
   go in `tests/`, each check naming its counter-case (`payment_succeed` being
   displayed as an anomaly; a list shrinking below the focus index; an empty
   `state`).

---

## 10. Acceptance criteria for the port

Written as manual tests on console. A port is accepted only if all twenty pass.

1. On launch the screen shows the data centre, the plan line "Plan: loading…"
   then the real plan, and the machine list.
2. The machine list appears **without waiting** for the plan line: cutting the
   network to `api.eu.shadow.tech` alone (or reading the log timestamps) must
   still let the list appear normally.
3. Every card carries the machine's name; a machine with no usable name shows a
   **translated** text, not a hardcoded French string.
4. No card shows a coloured dot until the server's state field has been
   identified; in particular, no card may display a grey "unknown" state as the
   permanent state of every machine.
5. **X** reloads the list: the counter at the bottom goes back through
   "Loading…" then returns to "N machine(s) of M".
6. Hammering **X** ten times in a row does not crash, duplicates no card and
   leaves the focus on a valid card.
7. Cut the Wi-Fi then press **X**: the previous list **stays displayed**, and an
   explicit error message (not "HTTP error 0") appears.
8. Restore the Wi-Fi then press **X**: the list reloads without a restart.
9. **Y** opens Settings; **B** from Settings returns to the list **without
   crashing** (that is exactly S63's symptom).
10. On returning from Settings the list is **reloaded** (an up-to-date state),
    from the **first** return onwards.
11. **B** on the list opens an application-exit confirmation whose cancel button
    does **not** mention a "stream".
12. **+** quits the application; its footer label is distinct from **B**'s.
13. **A** on a card opens the connection screen for that machine; the log shows
    the id and alias retained.
14. A **touch** on a card selects and activates it, exactly as A does; a
    vertical drag scrolls the list.
15. An account with **a single machine** and the "Automatic connection" setting
    **on**: the connection starts on its own ~500 ms after the list appears.
16. The same setup, but press **Y** within the second: you land in Settings and
    **no connection starts** on top of it (defect D6).
17. The same setup: leaving the stream brings you back to the list and **the
    auto-connect does not fire again**; you can reach Settings and change
    machine (that is the regression S62 fixed).
18. The "Automatic connection" setting **off**: no automatic connection, whatever
    the number of machines. The setting survives an application restart.
19. After returning from a stream, the state shown for the machine reflects the
    reload, **from the first return onwards** (defect D1).
20. Thirty list → stream → list round trips without a crash, and Atmosphère's
    `crash_reports/` stays empty.

---

*Written on 2026-08-27. Every claim in this document is verifiable at the line
cited; the claims about what the server replies actually contain come from
`/tmp/halyard/stderr.log`, not from reading a schema.*
