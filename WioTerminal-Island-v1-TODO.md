# Wio Terminal Island — v1 Build To-Do

**Device:** Seeed Wio Terminal (ATSAMD51 core, 512KB flash / 256KB RAM, 320×240 screen, 5-way switch + 3 buttons, onboard LIS3DHTR accel + mic + buzzer + light sensor, microSD, RTL8720DN Wi-Fi/BLE, 2× Grove + 40-pin header).
**Role:** Standalone field instrument. Runs with or without Scottina / Scottina Light / CanTick. Later interfaces with all three, but v1 depends on none of them.
**Scope:** Diagnostics ONLY. No offensive or attack tooling. CAN TX/RX permitted **solely** for heartbeat ACK/replies required to stay valid on a live bus — no injection, no attack frames.

---

## Hardware Reality Constraints (read before building)

- **No runtime code paging.** SAMD51 executes from internal flash; code cannot be overlay-loaded from SD. All screens compile into **one static firmware image** — 512KB flash holds many screens; the real limit is 256KB **RAM** (live buffers: FFT windows, framebuffer, Wi-Fi stack). SD is for **data only** (config, DBC, PGN tables, logs, calibration).
- **Wi-Fi is limited by the RTL8720DN AT-firmware layer.** Available: scan SSIDs, connect, basic TCP/HTTP, station list when acting as AP. **Not** available: ARP sweep, port scan, packet capture, host enumeration. Do not promise network-mapping features.
- **Wi-Fi radio stays dark in normal operation.** No confirmed upstream use case for the Terminal — it only wakes for the captive portal (DBC/PGN load). Upstream is always some other node's job.
- **CAN is external.** SAMD51's native CAN is not cleanly broken out on the 40-pin header → use **Grove MCP2515** (SPI). No I²C address contention for it.
- **I²C addresses are not globally unique.** Autodetect narrows the candidate list; user disambiguates on collision.

---

## 0. Foundations (do this first)

- [ ] Single static firmware skeleton: boot → I²C bus scan → top menu of screens.
- [ ] **Screen framework:** a base `Screen` type with `enter()/tick()/exit()/onButton()`; menu instantiates/switches screens. Mirror Scottina's per-screen model, MCU-appropriate.
- [ ] **Peripheral-autodetect → screen-enable:** bus scan maps detected I²C addresses to known-sensor screens; screens light up only when their hardware is present.
- [ ] **Address-collision handling:** when an address maps to multiple known devices, present a narrowed pick-list; persist the choice to SD.
- [ ] Scope banner in code comments: diagnostics-only + CAN-heartbeat-only exception + link to this file.

---

## 1. Config Model (three tiers — build the plumbing early)

Config split by how often it changes and how painful it is to type on a 5-way.

- [ ] **Tier 1 — Auto-detected** (the "hands-on scan" magic): I²C addresses (bus scan), UART baud (autobaud), CAN bitrate (autobaud).
- [ ] **Tier 2 — On-device pick-from-list** (per session, buttons only, no phone): I²C device disambiguation, CAN filter/IDs to watch, start/stop logging + format, which DBC/PGN table to apply, force-decode override.
- [ ] **Tier 3 — Authored off-device** (rare, painful to type): Wi-Fi creds, DBC files, PGN tables, dash layout. **SD JSON is the single source of truth.**
- [ ] SD JSON loader/parser with sane defaults when a file is missing or malformed (never hard-fail on bad config).

---

## 2. Discovery Spine (scanning-first — highest priority)

Three screens sharing one pattern: **listen, identify, show what's there.** None requires config to start.

- [ ] **I²C scanner:** scan bus, list addresses, map to known devices, drive the autodetect in §0. This *is* the "features appear as peripherals are detected" mechanism.
- [ ] **UART autobaud + dump:** listen on an unknown serial line, lock baud when framing goes clean, stream decoded bytes. On lock-fail → user picks baud from a list.
- [ ] **CAN passive sniffer (raw):** landing mode for the CAN screen. Autobaud the bitrate, then show IDs seen, per-ID frame rate, bus load %. Passive only (no TX in this mode).

---

## 3. SD Raw Logger (the black-box core — everything sinks here)

Raw capture is the source of truth; all decoders are views over it (live or replayed).

- [ ] **Log raw CAN frames** to SD, timestamped, decoder-agnostic. Always-on capability under the CAN screen.
- [ ] Shared logger primitive reused by UART/serial/I²C sensor screens.
- [ ] On-device **log browser:** list, preview, manage captures.
- [ ] Ring/size management so a long capture can't fill the card silently; surface remaining space.
- [ ] **Disconnect/standalone resilience:** logging continues with no external dependency (no brain required).

---

## 4. CAN Screen — Sniff / Decode / Dash

Grove MCP2515. One screen, three modes cycled by button. All modes feed the §3 raw logger.

- [ ] **Bitrate autobaud** → **manual pick on fail** (passive; needs bus traffic to lock — acceptable, N2K buses always chatter).
- [ ] **Mode 1 — Sniff (raw):** landing mode from §2 (IDs, rates, bus load). Zero config.
- [ ] **Mode 2 — Decode:** live scrolling signal list with units, per loaded table + selection.
- [ ] **Mode 3 — Dash:** curated signal subset as auto-laid-out gauge tiles (N signals → N tiles, no manual placement) with **per-tile freshness indicator** (stale = obvious).
- [ ] **CAN heartbeat TX/RX** (the sanctioned exception): ACK + required heartbeat replies to stay valid on a live bus. Diagnostics-only guard: no arbitrary frame injection path in the UI.

---

## 5. Decode Layer — Two decoders, one raw sniffer

Auto-route by ID width; user can override.

- [ ] **Router:** 11-bit standard ID → DBC decode; 29-bit extended ID → N2K/PGN decode.
- [ ] **User override:** force "decode this as DBC / as PGN" and pick which table, for oddball buses.
- [ ] **11-bit path — DBC decode:** for manufacturer-proprietary standard-ID data (e.g. Volvo Penta / Yamaha proprietary).
- [ ] **29-bit path — N2K decode:**
  - [ ] Unpack the 29-bit header → extract **PGN + source address** (priority/PGN/SA from arbitration field).
  - [ ] **Fast-packet reassembly** (REQUIRED for v1 — nav needs it): buffer multi-frame sequences keyed by PGN+source, reassemble before decode.
  - [ ] Single-frame PGN decode (engine/power easy wins — validate the pipe first).
  - [ ] PGN registry lookup → signals with SI-correct units.

**v1 test signal set (small, marine-real, proves the whole stack):**
- [ ] **Nav:** lat, lon, SOG, heading, COG (forces fast-packet on day one — PGN 129025/129029 etc.).
- [ ] **Engine:** RPM, coolant temp, oil pressure (single-frame — easy wins).
- [ ] **Power:** battery voltage / state (single-frame).
- [ ] Grow the PGN coverage later once these prove out.

---

## 6. PGN + DBC Registries (file-fed, same pattern)

Two registries, one mental model: drop file on SD → live at boot.

- [ ] **Built-in Canboat PGN table** for the common marine PGNs (nav/engine/power subset for v1).
- [ ] **User proprietary PGN tables:** `/pgn/*.json` (Canboat-style JSON). Merge by PGN number; **user entry wins on collision** (extend/correct built-ins).
- [ ] **DBC registry:** `/dbc/*.dbc`, loaded the same way for 11-bit proprietary decode.
- [ ] Graceful handling of malformed/partial table files (skip + log, don't crash the screen).
- [ ] *(Off-device, separate app — NOT on this firmware):* PDF→Canboat-JSON scraper/converter that Scott builds standalone. The Terminal only ever consumes clean canonical JSON.

---

## 7. Replay (decode over SD, not live bus)

- [ ] Replay screen = the **same §5 decoders** pointed at an SD raw log instead of the live bus.
- [ ] Re-decode any capture through DBC **or** PGN; improved decoders can re-run old logs.
- [ ] Basic transport: play / pause / step / seek through a capture.

---

## 8. Sensor Screens (onboard + Grove, autodetect-driven)

- [ ] **IMU / vibration:** onboard LIS3DHTR accel — RMS, peak, live trace. Machinery anomaly-watching in real time.
- [ ] **Audio FFT:** onboard mic — real-time FFT (ArduinoFFT path), spectral display, log dominant peaks (bearing wear / cavitation / imbalance signatures). Confirm SAMD51 headroom for live rate; fall back to log-then-analyze if needed.
- [ ] **Generic I²C sensor loggers:** whatever Grove sensor is detected (temp/humidity/pressure/light/etc.) → live read + SD log. Screens appear via §0 autodetect.
- [ ] *(Optional add-ons, no dangling wires — keyed Grove/40-pin only):* Grove 6/9-DOF IMU for attitude, Grove GNSS for position/track. Only if a spatial mission is confirmed.

---

## 9. Wi-Fi Utility (honest scope only)

- [ ] Scan + list SSIDs.
- [ ] Connect to a chosen SSID (creds from Tier-3 SD JSON, or on-device entry).
- [ ] Show own IP / connection status.
- [ ] **Explicitly out of scope:** host/IP enumeration, port scan, packet capture (AT-layer can't do it — document so no one "helpfully" adds it).

---

## 10. Captive Portal (rare-use, narrow job)

**Only** a DBC/PGN loader + signal pre-selector. Not runtime config, not a dashboard builder.

- [ ] Spin up AP + minimal HTTP form on demand (not at boot).
- [ ] Upload `.dbc` / `.json` (PGN) to SD.
- [ ] Tick which signals should surface on-device → writes a small manifest, e.g. `/dbc/<name>.sel.json` → `{ "signals": [...] }`.
- [ ] Device reads DBC/PGN + its `.sel.json` at boot → only ticked signals decode/display (full auto-list still available on-device).
- [ ] **No** signal→tile layout in the portal (that pain stays out; dash auto-lays-out).

---

## Build Sequence (each proves out before the next depends on it)

1. **Foundations + screen framework + I²C autodetect** (§0)
2. **Config plumbing** (§1)
3. **Discovery spine** — I²C / UART / CAN sniff (§2)
4. **SD raw logger + browser** (§3)
5. **CAN screen, Sniff mode + autobaud** (§4 partial)
6. **Decode layer: single-frame first** (engine/power) (§5 partial + §6 registries)
7. **Fast-packet reassembly → nav** (§5 completion)
8. **CAN Decode + Dash modes** (§4 completion)
9. **Replay** (§7)
10. **Sensor screens** (§8)
11. **Wi-Fi utility** (§9)
12. **Captive portal** — parallel track, needed once real DBC/PGN files exist (§10)

---

## Definition of Done (v1)

- [ ] Boots to a menu; screens auto-appear as I²C peripherals are detected; collisions resolved by user pick.
- [ ] CAN screen autobauds (manual fallback), sniffs raw, logs raw to SD always.
- [ ] 11-bit→DBC and 29-bit→N2K routing works; user can force a table.
- [ ] **Fast-packet reassembly works** — nav (lat/lon/SOG/heading/COG) decodes correctly.
- [ ] Engine + power single-frame PGNs decode with correct SI units.
- [ ] Built-in Canboat + proprietary `/pgn/*.json` merge, user-wins-on-collision.
- [ ] Replay re-decodes an SD capture through DBC and PGN.
- [ ] IMU + audio-FFT screens run; SD logging works and survives standalone (no external brain).
- [ ] Wi-Fi utility limited to scan/connect/IP; captive portal loads DBC/PGN + pre-selects signals only.
- [ ] Runs completely standalone — no dependency on Scottina, Scottina Light, or CanTick.
- [ ] Diagnostics-only scope documented in code + README; CAN TX limited to heartbeat/ACK.
