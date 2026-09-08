# Karen — Validation Report: Rotation Feature (`rotate-cmd`)

- **Branch:** `rotate-cmd` @ `a1c2387` (clean tree, 4 commits ahead of `origin/main`)
- **Date:** 2026-09-08
- **Target release:** v1.1.0
- **Diff base:** `origin/main`
- **Feature commits:** `ef53b5a` (FYROTATE serial), `15656ef` (docs), `869bde4` (hold gesture + persistence), `a1c2387` (docs)

**Companion:** v1.0.0 CrowPanel-port SHIP report preserved verbatim at
`docs/port/karen-validation-report.md` (unmodified).

---

## Executive Summary

**Overall Status:** **PASS — SHIP**
**Overall Risk:** Low
**Confidence:** High (logic, builds, persistence, wire contract); Medium on gesture feel/timing (hardware-pending).

This is a small, well-scoped feature. Every piece of evidence I asked for lines
up. The touch state machine is the part that had to be right, and it is: tap and
press-and-hold are mutually exclusive per contact with no double-fire path I
could construct. Persistence is conservative (SPIFFS single byte, tmp-write +
read-back validation + atomic promote, fallback to compile default on any
error). Wire contract is untouched. Builds are clean on both boards. Docs
match code line-for-line.

The only items below `SHIP` are one WARN (FYROTATE returns JSON while FYSCREEN
is silent — internally consistent with other JSON events, but the
"consistent with FYSCREEN" framing in the brief was misleading) and a
HARDWARE-PENDING note covering what only the physical CrowPanel can prove.

---

## Detailed Results

### Check 1 — Builds — **PASS**

Both environments rebuilt clean. Sizes pasted below.

| Env | RAM (used / total) | Flash (used / total) | vs `origin/main` |
|---|---|---|---|
| `crowpanel` | 88 844 / 327 680 (27.1%) | 1 367 161 / 3 145 728 (43.5%) | +16 B RAM, +1 140 B Flash |
| `cyd` | 80 768 / 327 680 (24.6%) | 1 592 825 / 3 080 192 (51.7%) | +8 B RAM, +1 104 B Flash |

Build evidence (PlatformIO 6.1.19, espressif32@^6.3.0):
- `pio run -e crowpanel` → `SUCCESS in 00:00:02.440`
- `pio run -e cyd` → `SUCCESS in 00:00:02.359`

No warnings. Footprint delta is the four new state variables
(`cydTouchDownAt`, `cydTouchHoldFired`, `cydTouchTapArmed`,
`cydLastTouchMs`/existing — re-used, `cydLastTouchMs` is unchanged) plus the
two new functions `fySaveRotation` / `fyLoadRotation` (~80 lines), the FYROTATE
branch in `cydHandleCommand`, the constants (`FY_ROTATE_HOLD_MS`,
`FY_ROTATION_FILE`, `FY_ROTATION_TMP`), and the four-line `setup()` reorder.
Sizes are consistent with the diff size (107 net lines of code change).

### Check 2 — Touch state machine (highest priority) — **PASS**

State variables in `cydTouchTick()`:

| Var | Meaning | Reset on release |
|---|---|---|
| `cydLastTouchDown` (LTD) | previous tick's pressed bool | always set to `down` at function end |
| `cydTouchDownAt` (TDA) | millis of contact start; `0` = not in contact | set to `0` on release and on flash dismissal |
| `cydTouchHoldFired` (THF) | hold already handled this contact | set to `false` on new contact and on release |
| `cydTouchTapArmed` (TTA) | tap candidate, until hold fires | set to `false` when hold fires and on release |
| `cydLastTouchMs` (LTM) | millis of last accepted contact start (debounce gate) | not reset on release; gates new contact start only |

Three branches, in order:
1. **NEW** (`D=1, LTD=0`): if within debounce, drop contact. Else if `cydFlashActive`, dismiss flash and consume contact (TDA=0, TTA=0). Else arm tap and start timer (TDA=now, TTA=true).
2. **HOLD_FIRE** (`D=1, LTD=1, TDA≠0, !THF, age≥FY_ROTATE_HOLD_MS`): rotate once, persist, disarm tap (`THF=true, TTA=false`).
3. **RELEASE** (`D=0, LTD=1`): if `TTA && !THF`, fire tap (cycle screens + debug print). Always clear state.

**Scenarios I traced on paper** (results):

| # | Scenario | Result |
|---|---|---|
| A | Short tap (no flash) | Screen cycles exactly once. ✅ |
| B | Press-and-hold ≥900ms | Rotates once; release is inert (`TTA=false`). ✅ |
| C | Hold → keep holding → release | Rotation fired once; release is inert. ✅ |
| D | Tap → re-press within 300ms | Second press dropped by debounce (matches pre-feature behavior — `cydLastTouchMs` gate is unchanged). ✅ |
| E | Flash overlay active → touch | Flash dismissed; no rotation, no screen-cycle. ✅ |
| F | Flash dismiss → release → fresh hold | Flash state cleared; subsequent press-and-hold rotates normally. ✅ |
| G | Quick tap → quick tap (within 300ms) | Second tap dropped (debounce). ✅ |
| H | Touch held 890ms then released | Treated as slow tap → cycles screen (no rotation). ✅ |
| I | Touch held 900ms then released | Rotates at threshold; release is inert. ✅ |
| J | Touch held 5s then released | Rotation fires exactly once; release is inert. ✅ |

**Double-fire proof.** A hold only fires in branch 2, which requires `!cydTouchHoldFired`. Once it fires, `THF=true`, so on subsequent ticks in the same contact the branch is skipped. The release in branch 3 only fires a tap if `cydTouchTapArmed && !cydTouchHoldFired`; the hold branch sets `TTA=false` before returning, so the tap is disarmed. **No path produces both a rotation and a screen-cycle from one contact.** A fresh press is the only path back to `THF=false`/`TTA=true` (branch 1, debounce-gated).

**Flash overlay priority.** Flash dismissal is checked inside branch 1 (new contact) BEFORE the tap-arm path, and the contact is consumed (`TDA=0, TTA=0`). Subsequent ticks while still pressed see `D=1, LTD=1, TDA=0`, so branch 2's `TDA≠0` check fails — no rotation mid-flash-dismissal-press. Tap on release: `TTA=false` — inert. ✅

**Fresh press after release.** Branch 3 always sets `TDA=0, THF=false, TTA=false`, and branch 1 starts a new `TDA=now, TTA=true` (subject to debounce). ✅

### Check 3 — Persistence — **PASS**

Mechanism: SPIFFS file `/rotation`, one ASCII byte `'0'..'3'`. Same atomic-promote pattern as `fySaveSession()` (`FY_SESSION_FILE`/`FY_SESSION_TMP`):

```
write tmp -> read-back validate -> atomic promote (rename or copy+remove)
```

**Lifecycle:**
- `setup()` reordered: `SPIFFS.begin(true)` now runs BEFORE `cydInit()`. With `fySpiffsReady=true`, `cydTftRotation = fyLoadRotation();` runs immediately before `cydInit()`. `cydInit()` → `cydInitDisplay()` → `tft.init(); tft.setRotation(cydTftRotation);`. **Persisted orientation is applied at first paint.** ✅
- Fallback: if `fySpiffsReady=false`, file absent, file empty, or byte outside `'0'..'3'` → returns `CYD_TFT_ROTATION` (compile default). `dualPrintf("[flockyou] rotation %u restored from SPIFFS\n", n)` only on actual restore (avoids noise on default).
- Save fires on **every change**:
  - FYROTATE command (both `,next` and `,0-3`) → `fySaveRotation(cydTftRotation)` after `cydSetDisplayRotation(target, true)`.
  - Touch hold → `fySaveRotation(cydTftRotation)` after `cydSetDisplayRotation(...)`.
- CYD boot-button path is **deliberately not** changed: it still calls `cydSetDisplayRotation((cydTftRotation+1)%4, true)` with no persist (matches "boot-button rotation cycle is unchanged and does not persist" in docs).

**Failure modes I checked:**
- `f.write` fails → tmp file empty, read-back validation fails → return `false`, no promote, no crash. `saved:false` returned on FYROTATE path.
- `fySpiffsReady=false` → returns `false` early. Caller still applies rotation in RAM; logs `(save failed)` on gesture path or `saved:false` on FYROTATE path.
- Corrupt byte (out of range) on next boot → falls back to `CYD_TFT_ROTATION`. Worst case is one boot at default orientation, never a boot failure (deliberate, as the in-source comment notes).

**Concurrency.** Single-threaded loop with `cydHandleCommand` and `cydTouchTick` running sequentially in `loop()`. No preemption concerns. ✅

### Check 4 — Wire contract — **PASS**

`git diff origin/main..origin/rotate-cmd -- main.cpp` filtered for wire-contract
identifiers (`CYD_PAIR_NAME`, `protocol_version`, UUIDs, BLE characteristics,
detection paths, pairing paths):

```
(no matches)
```

- `CYD_PAIR_NAME` = `"CYD-Flock-You"` (`main.cpp:38`) — **unchanged**.
- `CYD_PROTOCOL_VERSION` = `1` — **unchanged**.
- BLE UUIDs `6E400001/2/3-B5A3-F393-E0A9-E50E24DCCA9E` — **unchanged**.
- `BLEDevice::init(CYD_PAIR_NAME)` (`main.cpp:2006`) — **unchanged**.
- `cydBleWriteBytes` null-guard (`cydBleReady`/`cydBleClientConnected`/`cydBleTx`) — unchanged. The port-notes claim "safe because `cydBleWriteBytes` is null-guarded before BLE init" is correct — verified at `main.cpp:1992`.
- `cydRecordDetection`, `cydEmitPairStatus`, `emitDetectionJSON`, `fyAddDetection` — none touched.
- `cydButtonTick()` (CYD boot button) — **unchanged** (still `HIGH→LOW` edge, 250 ms debounce, no persist). Matches docs.
- `git diff --stat`: `main.cpp` 147 insertions / 8 deletions confined to: defines (`FY_ROTATION_FILE`/`TMP`), 3 static state variables, `fySaveRotation`, `fyLoadRotation`, `cydTouchTick` body, `FYROTATE` command branch in `cydHandleCommand`, and 4 lines of `setup()` reorder. No other file's logic touched.

### Check 5 — FYROTATE command — **PASS**

`main.cpp:1882-1898`:

| Input | Behavior | Evidence |
|---|---|---|
| `FYROTATE,next` | target = `(cydTftRotation+1)%4`; rotate, redraw, persist; reply `{"event":"rotate","rotation":<n>,"saved":<bool>}` | ✅ |
| `FYROTATE,0` | target = 0; same path | ✅ |
| `FYROTATE,1` | target = 1 | ✅ |
| `FYROTATE,2` | target = 2 | ✅ |
| `FYROTATE,3` | target = 3 | ✅ |
| `FYROTATE,4` | usage error (out of range) | ✅ |
| `FYROTATE,-1` | usage error (`-` outside `'0'..'3'`) | ✅ |
| `FYROTATE,` | usage error (`arg[1]=='\0'`) | ✅ |
| `FYROTATE` (no arg) | usage error (`arg[0]=='\0'`) | ✅ |
| `FYROTATE,A` | usage error (`A` not in range) | ✅ |
| `FYROTATE,nextfoo` | usage error (strcmp fails, then `n` not in range) | ✅ |
| `FYROTATE,1,2` | usage error (`arg[2]==','` not `'\0'`) | ✅ |

Error response:
```json
{"event":"rotate_error","error":"usage","usage":"FYROTATE,next | FYROTATE,<0-3>"}
```

Success response (e.g., after `FYROTATE,2`):
```json
{"event":"rotate","rotation":2,"saved":true}
```

`saved` is `false` only when `fySaveRotation` returned `false` (SPIFFS unavailable or write failure). Rotation is still applied in RAM. The README correctly notes "`saved:false` only means SPIFFS is unavailable (rotation still applies until reboot)".

Redraw: `cydSetDisplayRotation(target, true)` — `true` triggers `tft.fillScreen(CYD_COLOR_BG)` + `cydDrawUi(true)`. ✅

**WARN — "response format consistent with FYSCREEN".** The brief asked me to verify response format consistency with `FYSCREEN`. `FYSCREEN,next` (`main.cpp:1877-1881`) is **silent** — it sets `cydScreen` and redraws without emitting any JSON. `FYROTATE` emits a JSON response. These two are not literally consistent; they are both internally consistent with their own design (FYSCREEN follows the "no-response command" pattern; FYROTATE follows the JSON-event pattern shared by `pair_status`, `detection`, `touch_status`, etc.). The README accurately documents FYROTATE's actual response and does **not** claim consistency with FYSCREEN. The wording in the brief was the only place "consistent with FYSCREEN" appeared — recommend dropping it from the spec. This is **not a code defect**.

### Check 6 — Docs — **PASS**

`README.md` (touch table + serial commands table + rotation paragraph):

| Doc claim | Code reality | Verdict |
|---|---|---|
| Tap cycles screens / dismisses flash | `cydTouchTick` RELEASE branch fires `cydScreen++` when `TTA && !THF`; flash dismissed in NEW branch | ✅ |
| Tap-and-hold are mutually exclusive per touch | Verified in state-machine trace (Check 2) | ✅ |
| Hold threshold ~0.9s (`FY_ROTATE_HOLD_MS`) | `#define FY_ROTATE_HOLD_MS 900` in `board_config.h` (FY_UI_BUILD) | ✅ |
| Releasing after a hold is inert | `TTA=false` after HOLD_FIRE; RELEASE branch skips | ✅ |
| Quick tap cycles screens on release, exactly as before | RELEASE branch on `TTA && !THF` is the same `cydScreen = (CydScreen)(((uint8_t)cydScreen + 1) % SCREEN_COUNT); cydDrawUi(true);` as pre-feature | ✅ |
| Flash overlay priority | NEW branch checks `cydFlashActive` BEFORE arming tap | ✅ |
| FYROTATE syntax: `FYROTATE,next` or `FYROTATE,<0-3>` | `main.cpp:1882-1889` matches exactly | ✅ |
| Response: `{"event":"rotate","rotation":<0-3>,"saved":true|false}` | `main.cpp:1896-1897` matches exactly | ✅ |
| Persists to SPIFFS `/rotation`, single ASCII digit, defaults to `CYD_TFT_ROTATION` | `FY_ROTATION_FILE`, `fySaveRotation`, `fyLoadRotation` | ✅ |

`docs/port/port-implementation-notes.md` (CrowPanel notes):

| Doc claim | Code reality | Verdict |
|---|---|---|
| Rotation via FYROTATE serial and touch press-and-hold (~0.9s, `FY_ROTATE_HOLD_MS`) | Both paths implemented | ✅ |
| Persists to SPIFFS `/rotation`, one ASCII digit | `fySaveRotation` / `fyLoadRotation` | ✅ |
| Restored at boot before `cydInit()`'s first `setRotation()` | `setup()` SPIFFS → `fyLoadRotation()` → `cydInit()` → `cydInitDisplay()` → `tft.setRotation(cydTftRotation)` | ✅ |
| Tap-vs-hold mutually exclusive per contact | State machine proof (Check 2) | ✅ |
| CYD boot-button rotation cycle unchanged and does not persist | `cydButtonTick` unchanged, no `fySaveRotation` call | ✅ |
| `setup()` reorder: SPIFFS before `cydInit()` | Verified | ✅ |
| Safe because `cydBleWriteBytes` is null-guarded | `cydBleWriteBytes` checks `cydBleReady` at line 1992 | ✅ |
| Hardware checklist updated with hold-gesture step | "Tap cycles … press-and-hold (~0.9s) rotates the orientation; release after a hold does not cycle screens" present | ✅ |

**Pre-existing docs comment.** `docs/port/port-implementation-notes.md` has a comment that says "Tap and press-and-hold are mutually exclusive per contact in `cydTouchTick()`: the screen-cycle tap fires on release, the hold fires once at the threshold — so a hold never cycles a screen first and releasing after a hold is inert." This is exactly the claim I verified above. ✅

---

## Defect Summary

| # | Severity | Item | Status |
|---|---|---|---|
| — | — | (none) | — |

No Critical, High, Medium, or Low defects found.

### Informational

- **FYROTATE response format is JSON; FYSCREEN is silent.** Internally consistent with other JSON events; brief's "consistent with FYSCREEN" framing was misleading. Recommend brief amendment.
- **CYD boot button still does not persist.** Intentional per docs (it cycles through rotation in-session only). If you want CYD parity with CrowPanel here, that's a one-line `fySaveRotation(cydTftRotation)` addition to `cydButtonTick()` — but the feature spec explicitly chose not to, and the docs state it. **No action required for v1.1.0.**
- **No new test harness added.** Project has no existing unit-test infrastructure (verified by absence of `test/` in `platformio.ini`). Logic was validated by static trace + rebuild; live-device validation is HARDWARE-PENDING.

---

## HARDWARE-PENDING

The following cannot be proven without the physical board on the bench. The
remote friend with browser-based serial access is the only one who can verify
these.

1. **Gesture feel.** 900 ms is "well above a deliberate tap (~100–200 ms)" per
   in-source comment, but feel is subjective. If the friend reports it feels
   too slow / too twitchy, `FY_ROTATE_HOLD_MS` is a single constant in
   `board_config.h` (`#if FY_UI_BUILD` block, single board definition so
   boards cannot drift apart). 700–1000 ms is the realistic range.
2. **Hold gesture actually fires on the panel.** Static trace proves logic;
   only the physical FT6236 + touch stack proves `boardTouchPressed()` stays
   true for the full 900 ms without spurious release events. Spurious
   release/re-press within the hold window would re-arm `TDA` and could
   cause either a delayed rotation (if re-press extends hold timer) or no
   rotation at all (if the re-press drops via debounce). If observed, the
   touch driver's noise profile needs investigation.
3. **Flash dismissal does not fire rotation on long-held dismiss-press.** Logic
   says it can't (NEW branch sets `TDA=0` on flash dismissal; HOLD_FIRE branch
   requires `TDA≠0`). Worth a 3-second hold while the flash is active to
   confirm on hardware.
4. **SPIFFS persistence across actual power cycle.** `SPIFFS.begin(true)`
   formats on first boot; subsequent boots read `/rotation` and restore. Logic
   is correct; physical confirmation is one power-cycle test.
5. **FYROTATE over the browser serial console.** Friend-side browser serial
   must send `FYROTATE,next` + `\n` (or `\r\n`); both are handled. JSON
   response should appear in the same console.
6. **Layout under each of the 4 rotations.** Code calls `cydSetDisplayRotation`
   → `tft.setRotation` → `cydDrawUi(true)`; layout is portrait-aware per
   docs claim. Visual confirmation across all 4 orientations on the actual
   320×480 panel is a hardware test.

None of the above are blockers. They are observations the friend should make
on first physical flash and report back.

---

## Release Recommendation

**Ready for Production.**

Reasoning:
- All 6 requested checks PASS.
- No Critical/High/Medium/Low defects.
- Footprint is trivial (+~1.1 KB Flash per env).
- Wire contract preserved; CYD behavior unchanged for users who don't use the
  new commands/gesture.
- The one WARN is a brief-framing issue (FYROTATE is JSON, FYSCREEN is silent),
  not a code defect — and the README accurately documents the actual
  behavior.
- HARDWARE-PENDING items are routine for embedded UI work and not
  release-blockers; the friend's first power-cycle test will close them.

**SHIP for v1.1.0.**

Tag suggestion: `v1.1.0-crowpanel` after merge of `rotate-cmd` into `main`.