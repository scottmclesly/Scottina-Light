<img width="2567" height="1925" alt="ScottinaLight" src="https://github.com/user-attachments/assets/cb5c6ab0-3069-42e9-82da-f95d00921cec" />

# Scottina Light

**Pocket digital beast of burden.** A standalone diagnostics instrument for the
Seeed Wio Terminal — the little sibling of [Scottina](https://github.com/scottmclesly/Scottina).

Same kiosk language as the mother project — CRT phosphor palette, semiotic
pictograms, navigable tiles that appear as their hardware is detected — on a
320×240 panel driven by a 5-way switch instead of a touchscreen.

It boots, scans the bus, and logs on its own. It depends on Scottina, Scottina
Light, and CanTick for nothing.

---

## Scope: diagnostics only

This firmware is a **passive field instrument**. That constraint is deliberate,
and it is enforced in the code rather than left to good intentions — see
[`include/scope.h`](include/scope.h).

**Permitted**

- Passive observation: I²C bus scan, UART listen/autobaud, CAN passive sniff,
  onboard sensor reads.
- Raw capture to SD, and decode/replay of that capture.

**The one sanctioned transmit exception**

- CAN heartbeat / ACK replies required to remain a valid node on a live bus.
  There is deliberately no code path — and no UI affordance — for composing or
  injecting an arbitrary CAN frame.

**Explicitly out of scope, do not add**

- Any offensive or attack tooling; arbitrary frame injection.
- Network mapping (ARP sweep, port scan, packet capture, host enumeration). The
  RTL8720DN AT-firmware layer cannot do these anyway; the Wi-Fi surface is
  limited to scan / connect / report-own-IP.

---

## Hardware

| | |
|---|---|
| Board | [Seeed Wio Terminal](https://amzn.to/3SYBvaE) |
| MCU | ATSAMD51P19 — 512 KB flash, 256 KB RAM |
| Display | 320×240 TFT |
| Input | 5-way switch + 3 buttons (A / B / C) |
| Onboard | LIS3DHTR accelerometer, mic, buzzer, light sensor, microSD, RTL8720DN Wi-Fi/BLE |
| Expansion | 2× Grove, 40-pin header |
| CAN | External Grove MCP2515 over SPI (the SAMD51's native CAN is not cleanly broken out) |

Two I²C buses are scanned: `Wire` (Grove) and `Wire1` (internal).

---

## Status

`v1-foundation`. The skeleton, the discovery spine, and the black-box logger are
real; the decode layer is not yet written.

**Working**

- Screen framework (`enter` / `tick` / `exit` / `onButton`) with a nav stack.
- Boot splash — phosphor rain, wordmark, staged progress bar.
- **I²C scanner** — two-pass scan across both buses, a known-device registry,
  and `WHO_AM_I`-style verification to resolve address collisions. Unresolved
  collisions raise a pick-list; the choice is persisted to SD.
- **Autodetect → tile.** Tiles materialise only when their hardware answers, and
  the launcher re-scans on tick, so hot-plugged hardware shows up on its own.
- **Vibration** — live RMS / peak / trace off the onboard LIS3DHTR, with SD logging.
- **Serial** — UART autobaud (marine-first ordering: NMEA 0183 at 4800, AIS at
  38400), byte dump, manual baud pick on lock failure.
- **Logs** — on-device browser over the SD captures.
- **Raw logger** — rolling files, size cap, and a minimum-free-space floor so a
  long capture cannot silently fill the card.
- **Config** in three tiers (see below), never hard-failing on a missing or
  malformed file.
- Boot **smoke self-test** over serial.

**Not yet implemented**

- CAN sniff / decode / dash — the CAN tile currently reports presence only.
- DBC + PGN registries, 11-bit → DBC and 29-bit → N2K routing, fast-packet
  reassembly.
- Replay of an SD capture through the decoders.
- Audio FFT, Wi-Fi utility, captive-portal DBC/PGN loader.

The full specification and build sequence live in
[`WioTerminal-Island-v1-TODO.md`](WioTerminal-Island-v1-TODO.md).

---

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```bash
pio run                 # build
pio run -t upload       # flash
pio device monitor      # 115200 baud
```

Adjust `upload_port` / `monitor_port` in [`platformio.ini`](platformio.ini) to
match your board's serial device.

Dependencies are resolved by PlatformIO: `Seeed_Arduino_LCD`, the LIS3DHTR
driver, `Seeed Arduino FS`, `Seeed Arduino SFUD`, and `ArduinoJson` 7.

### The test hook

`platformio.ini` builds with `-DSL_TEST_HOOK=1`, which lets you drive the UI over
serial without hands on the device:

```
a b c   buttons A / B / C
u d l r 5-way directions
p       5-way press
```

**Drop that flag for a field build.**

---

## Navigation

**Launcher** — 5-way to move across the tile grid, `PRESS` or `A` to open a tile,
`B` to cycle the theme.

**Inside a screen** — `C` goes back. Per-screen actions are printed in the header
hint: `A` toggles logging on the Vibration screen, restarts the autobaud probe on
Serial, and so on.

---

## SD card

The card is optional — the firmware boots and scans without one — but it is
where config and captures live. Directories are created on first mount.

```
/config.json          Tier 3: authored off-device
/config/i2c.json      Tier 2: persisted I²C disambiguation choices
/config/ui.json       Tier 2: persisted theme
/logs/*.log           raw captures
```

### `/config.json`

Every key is optional. A missing, unreadable, or malformed file falls back to
built-in defaults with a note on serial — bad config never hard-fails the boot.

```json
{
  "wifi": { "ssid": "", "pass": "" },
  "can":  { "bitrate": 0, "cs_pin": 50 },
  "uart": { "baud": 0 },
  "log":  { "max_bytes": 8388608, "min_free_mb": 32 }
}
```

`bitrate` and `baud` of `0` mean **autobaud**. `cs_pin` defaults to the board's
`PIN_SPI_SS`, which is `50` on the Wio Terminal.

Config is split by how often it changes and how painful it is to type on a 5-way
switch: **Tier 1** is auto-detected (I²C addresses, UART baud, CAN bitrate),
**Tier 2** is picked on-device and persisted, **Tier 3** is authored off-device
and SD JSON is its single source of truth.

---

## Themes

Three CRT-inspired skins, ported from the mother project. Chrome is monochrome —
one phosphor colour and its shades — while the traffic-light status colours stay
vivid across all three, so a warning always reads as a warning.

- `green` — classic green phosphor
- `amber` — P3 amber phosphor
- `light` — sterile clinical white; chrome is greyscale, and only ok/warn/bad
  carry colour, for meaning alone

Cycle with `B` on the launcher. The choice persists to `/config/ui.json`.

---

## Serial output

Every boot prints a smoke self-test — LCD geometry, the internal accelerometer's
`WHO_AM_I`, the I²C scan census, SD state, config provenance, and a logger
write→close→stat→cleanup round-trip:

```
SMOKE begin Scottina Light v1-foundation
SMOKE lcd=PASS  320x240
SMOKE i2c.imu=PASS  Wire1 0x18 WHO_AM_I=0x33
...
SMOKE result=PASS checks=8 failures=0
SMOKE end
```

---

## Layout

```
include/
  scope.h              scope banner + product identity — read this first
  scottina_light.h     the whole public surface
  theme.h              palette struct
  pictograms.h         glyph enum
  tft_setup.h          TFT_eSPI config, force-included by the build
src/
  main.cpp             boot sequence + smoke self-test
  core_i2c.cpp         bus, probe, two-pass scan, known-device registry
  core_storage.cpp     SD mount, three-tier config, raw logger
  core_ui.cpp          input, chrome, nav stack
  screens.cpp          launcher + the five screens
  splash.cpp           phosphor rain
  theme.cpp            the three palettes
  pictograms.cpp       vector glyphs
```

Two details worth knowing before you touch the low level:

**The cold-begin phantom ACK.** On a bus with no pull-ups — nothing plugged into
Grove — the SAMD51 SERCOM reports a spurious ACK for whichever address is probed
first after a cold `begin()`. Verified on hardware: a cold probe of `0x42` ACKs,
and every probe after it correctly NACKs. `i2cbus::beginBus()` burns one throwaway
transaction to absorb that false positive, and it must run before anything scans.

**No enumerator may be named `RTC` or `ADC`.** CMSIS defines both as object-like
macros in `samd51p19a.h`, and the preprocessor does not respect enum scope.

---

## Relationship to Scottina

[Scottina](https://github.com/scottmclesly/Scottina) is the mother project — the
front panel. Scottina Light is its Wio Terminal sibling, sharing the visual
language and the per-screen model but none of the code and none of the runtime
dependency. Where a subsystem exists on both, the screen registry order and the
colour keys follow the mother.

Standalone by design.

---

by Scott McLeslie
