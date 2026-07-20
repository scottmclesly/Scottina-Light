# Scottina Light — Sensor Loggers & Alerts To-Do

**Device:** Scottina Light (Wio Terminal — ATSAMD51, 320×240 landscape screen, 5-way thumb switch + 3 top buttons, onboard **LIS3DHTR accel + mic + light sensor + buzzer**, microSD).
**Scope:** Diagnostics ONLY. These are passive machinery/environment monitors — no actuation.
**Design language:** Inherit Scottina Prime's DNA — Ron Cobb *Semiotic Standard* glyphs, phosphor CRT palette (green/amber/light; chrome monochrome, **ok/warn/bad vivid on every skin**), rounded-rect metric cards, alerts as **badge + flash, never modal**.

---

## Overview

Three loggers sharing **one framework** (sample → band-threshold → display → SD log). Vibration already exists; sound level and ambient light are added on the same pathway.

| Logger    | Sensor            | Units            | Primary alert edge        |
|-----------|-------------------|------------------|---------------------------|
| Vibration | LIS3DHTR accel    | RMS / peak (g)   | HIGH (excess vibration)   |
| Sound     | onboard mic       | relative → dB*   | HIGH (too loud) / LOW (silent) |
| Light     | onboard light sen | relative (lux-ish)| LOW ("went dark") / HIGH (too bright) |

\* Sound is uncalibrated-relative until a reference sample is captured (see §4). Never label the axis "dB SPL" without a valid CAL record.

---

## 0. Shared Logger Framework (build once, three consumers)

- [ ] Base `Logger` type: `sample()` → value, ring buffer for the live graph, SD sink, band-threshold evaluator, latch state.
- [ ] All three loggers register as tiles; each gets a **Semiotic-Standard glyph** in the existing pictogram family (new keys, graceful fallback to filled dot).
- [ ] Reuse Scottina's `rrect` card primitive + phosphor theme; no bespoke widgets.
- [ ] Per-logger `tick_interval` (vibration/sound fast, light slow) per the KioskSpeed dirty-rect guidance — only the graph band + value repaint between samples.

---

## 1. Screen Layout (320×240 landscape — graph-dominant)

Graph gets the real-estate; the metric card is compact.

- [ ] **Top bar:** logger title + Semiotic glyph + back/home.
- [ ] **Graph pane (dominant):** scrolling time-series, full width, most of the height.
  - [ ] **Two horizontal threshold markers** — HIGH (upper) and LOW (lower) — drawn across the graph in `warn`/`bad` accent.
  - [ ] **Park-to-disable:** a marker dragged to its **rail** (HIGH at the top edge, LOW at the bottom edge) = that threshold **disabled**. Marker position *is* the armed state — no separate toggle. Render a parked marker visibly "off" (muted/ghosted).
- [ ] **Compact metric card:** big bold current value + units, muted label, tiny state chip (armed/tripped/CAL). Scottina card styling.
- [ ] **Numeric threshold read-out:** show the grabbed marker's exact value while adjusting; show both band values at rest where space allows.

---

## 2. Grab-Cursor Threshold Interaction (5-way thumb switch)

The signature interaction — set thresholds by feel, on-device, no phone.

- [ ] **Up/Down** on the thumb switch moves a horizontal **cursor** vertically across the graph.
- [ ] **Press near a marker line** → *grabs* the nearest marker (HIGH or LOW, whichever the cursor is closest to). Grabbed marker follows Up/Down.
- [ ] **Press again** → *releases* the marker at its current position.
- [ ] Grabbed marker shows a **clear live numeric value** the whole time it's held.
- [ ] Dragging a grabbed marker fully to its rail commits the **disabled** state (§1 park-to-disable).
- [ ] Grab hit-tolerance tuned so it's easy to catch a line but hard to grab the wrong one; if both lines are near, prefer the closer.

---

## 3. Band Thresholds & Latching Alerts

- [ ] **Band model, all three loggers:** breach fires when the signal **exits the band** — above HIGH *or* below LOW. Each edge independently disable-by-parking (§1).
- [ ] **Latch:** once tripped, the alert **stays latched** (visual + buzzer) even after the signal returns inside the band — so a spike you missed is still evident. Explicit **dismiss** (a top button) clears the latch.
- [ ] **Alert cues (Scottina rule — badge + flash, never modal):**
  - [ ] Visual: state chip → `bad`, graph band flash, tile badge. No blocking dialog.
  - [ ] Audio: **buzzer** pattern on trip (Light has the buzzer Prime lacks — free cue). Distinct short pattern; silenceable on dismiss.
- [ ] **Thresholds are session-scoped** (RAM, per-reboot) — deliberately *not* persisted. Reset to a sane default (both rails / disabled) on boot.

---

## 4. Audio Reference Calibration (persists to SD)

Turns relative mic level into an approximate dB SPL via a single-point offset.

- [ ] **Capture flow:** play a known-dB reference source → Light samples its steady relative level → user enters/confirms the reference dB → store the **offset** (relative ↔ dB SPL).
- [ ] **Single-point = offset only.** Baked-in caveat: this is an approximation, not a lab meter; assumes linear response in the log domain. Surface this honestly in the UI.
- [ ] **CAL flag** on the sound tile when a valid record is loaded; axis/value switch from "relative" to "≈dB SPL". No flag → stays relative, never fakes dB.
- [ ] **Persist to SD** (e.g. `/cal/sound.json` → `{ "ref_db": …, "ref_level": …, "offset": …, "captured": <ts> }`). Loaded at boot — **calibration survives reboot even though thresholds don't** (deliberate asymmetry: cal is painful to redo, thresholds are quick to set).
- [ ] Malformed/missing cal record → fall back to relative cleanly, no crash.
- [ ] Re-run calibration overwrites the record (atomic tmp+rename, per Scottina's shared-data convention).

---

## 5. SD Logging (shared sink)

- [ ] Each logger streams timestamped samples to SD (reuse the raw-logger primitive from the CAN work).
- [ ] Log the **breach events** too (timestamped trip/clear), not just raw samples — this is the record the Tier-2 wireless push (§6) will eventually carry.
- [ ] Size/ring management so a long session can't silently fill the card.

---

## 6. Wireless Breach-Push to Scottina Prime — TIER 2 (parked, not v1)

Deferred by decision. Design intent captured so it drops in cleanly later.

- [ ] **Concept:** an **open, tile-agnostic push message** from Light → Prime carrying a breach event; Prime plays the alert on whatever screen is open (not tied to a specific tile).
- [ ] **Transport:** BLE the likely candidate (Light peripheral, Prime central) — Wi-Fi stays dark. *Not built in v1.*
- [ ] **Fallback that already works today:** breach events logged to SD (§5) ride the existing **Light Dock log-pull** to Prime on re-dock — deferred alerting for free, no new radio. Live wireless push is the Tier-2 upgrade over this.

---

## Build Sequence

1. **Shared framework + tile/glyph registration** (§0)
2. **Graph-dominant layout + compact card** (§1)
3. **Grab-cursor interaction + park-to-disable** (§2) — the hard/fun UI bit
4. **Band thresholds + latching buzzer/flash alerts** (§3)
5. **SD logging incl. breach events** (§5)
6. **Sound reference calibration + SD persistence** (§4)
7. *(later)* Tier-2 wireless breach push (§6)

---

## Definition of Done

- [ ] Vibration, sound, and light tiles all present, glyphed in the Semiotic-Standard family, matching Scottina card/phosphor styling.
- [ ] Each has HIGH + LOW markers; parking a marker at its rail disables that edge and reads visibly "off."
- [ ] Thumb-switch grab-cursor sets/moves/releases markers with a live numeric read-out.
- [ ] Breach = band-exit; alert **latches** with buzzer + flash + badge, never modal; dismiss clears.
- [ ] Thresholds session-scoped (reset on boot); **sound calibration persists to SD** and survives reboot.
- [ ] Sound shows "≈dB SPL" only with a valid CAL record, else honest relative level.
- [ ] All three log samples + breach events to SD.
- [ ] Runs standalone; Tier-2 wireless push documented but not built.
