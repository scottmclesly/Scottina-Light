# Light Dock — ToDo (for Claude Code)

**Goal:** when Scottina Light (the Wio Terminal sibling) is plugged into
Scottina Prime over USB, a hotplug **Light Dock** screen appears and runs a
fully automatic sync: **push wall-clock time**, **push enabled decode tables**
from the Prime table store, and **pull Light's SD logs** into Prime's
captures. Prime always wins; the user watches, they don't drive.

**Why it matters:** Light has no battery-backed RTC — standalone, its SD logs
are timestamped in "seconds since power-on," useless for fusion with anything
Prime saw. Docking makes Light's black-box logs *alignable* and delivers
decode tables without sneaker-netting the SD card. The dock is the sync
moment; Light stays standalone the rest of the time.

**Scope constraint (hard):** diagnostics only, both sides. The dock protocol
is provisioning + retrieval — it can set the clock, write tables/config, and
read closed log files. It deliberately cannot trigger transmission, start
captures remotely, or execute anything. Light's `scope.h` contract is not
weakened by docking.

**Cross-repo feature.** Work lands in two repos — kilodash (Prime side) and
`scottmclesly/Scottina-Light` (firmware side) — coupled *only* by
`DOCK-PROTOCOL.md`, exactly like PROTOCOL.md couples kilodash and CanTick.

---

## Locked design decisions (from the design session)

- **One direction of truth:** Prime's table store always wins; nothing is
  ever edited on Light. Conflicts aren't merged, they're overwritten.
- **Zero-interaction happy path:** dock → sync runs itself. The only
  controls: a **Re-sync** button and an **auto-pull-logs toggle** (the one
  step that costs time and space).
- **Auto-resume on redock:** a yanked cable mid-sync is not an error state
  to debug — atomic writes mean nothing half-exists, and the next dock
  resumes whatever's missing, automatically.
- **Two-distance UI, split screen:** top half is an **animation readable
  from across the room** (devices drawing together = syncing/complete;
  sad-face broken cable = interrupted); bottom half is a **boring
  session-only log** — timestamped lines, what made it and what didn't.
  Come closer only when the animation says to.
- **Session-only log, deliberately.** No rolling history. If a problem
  persists across docks, that's the signal to drop into real interfaces
  (SSH, the files on disk) — this screen answers "did the sync land?" and
  nothing more.

## Decision made while writing (flagging per protocol — veto if wrong)

**Transport: framed protocol over USB CDC serial; Light firmware mediates
all SD access. No USB mass-storage mode.**

Rationale: (a) Light's Wi-Fi surface is deliberately limited to
scan/connect/report-IP — the AT-firmware layer can't serve this, so USB is
the only dock link; (b) exposing the SD as MSC while the black-box logger
holds the same FAT volume is a two-writer corruption hazard, and "stop
logging because you're docked" throws away the black-box property exactly
when the boat is near a bench; (c) CDC runs at native USB full speed
(~1 MB/s effective — the nominal baud is ignored), so multi-MB log pulls
are seconds, not minutes. The cost is a small firmware protocol handler —
Phase 2 — instead of free filesystem access.

---

## Split: two lanes, three gates

Work is split across two Claude sessions — **Light** (this repo, firmware) and
**Prime** (kilodash). The phases already divide cleanly by repo, so the split is
mostly a matter of naming the gates and refusing to cross them.

| Lane | Owner | Phases |
|---|---|---|
| **L** | Light session (this repo) | Phase 1 — firmware dock handler |
| **P** | Prime session (kilodash) | Phase 2 — detection + sync engine; Phase 3 — the screen |
| **S** | Shared, either session, mirrored to both | `DOCK-PROTOCOL.md`, `dock-vectors.json`, Phase 0, Phase 4 docs |

### The gates

**G0 — the contract exists. ✅ CLEARED.** `DOCK-PROTOCOL.md` ratified by Prime
(commit `6a4fcdf`) byte-identical to Light's v0.1 draft. Two changes have landed
**since** ratification and need Prime's ack before the DRAFT banner drops:

- **`LIST` pagination** (Prime's catch, and a correct one). `proto_version` stays
  `1` — free now, a version bump later. Note the arithmetic differs from the
  original estimate: an entry costs **24 B unhashed**, not ~50, so `/logs/`
  overflows at **~42 files**, while hashed `/tables/` overflows at **~16–18** —
  which is where the 15–20 figure came from. Both are reachable; the conclusion
  was right either way.
- **`DELETE` timeout 30 s → 60 s.** Light is *not* comfortable with 30. An 8 MB
  log is 16–27 s to rehash over SPI-SD, and the uncached path is not exotic — it
  is the **resume path** (a dock interrupted between `GET` and `DELETE` means the
  next dock skips the pull and deletes into a cold cache). Auto-resume-on-redock
  is a headline feature; its timeout must survive its own happy path.

**G1 — the vectors exist. ✅ CLEARED.** `dock-vectors.json` delivered by Prime,
44 vectors. Light reviewed all 44 independently — decoded every frame, recomputed
every CRC, confirmed the three declared file digests — and **all 44 verified**.
The 7 `pins_open_behavior` vectors are **accepted, no vetoes** (see below).
Light extended the file to **47** (`vectors_version 0.2`) with the three LIST
pagination cases. Prime must re-run its engine against the new file.

**G2 — bench facts.** Phase 0's VID:PID + product string. Blocks Prime's
`devices.py` detection **only**. Does not block the firmware, and — because
`max_payload` is negotiated in `HELLO` rather than hardcoded — does not block the
wire format either. Run it whenever; it gates one function.

**G3 — integration.** Both lanes done, both green against the vectors, real
hardware on a real bench. Should be undramatic. If it isn't, the vectors were
incomplete, and *that* is the bug to fix.

### What runs in parallel

After G0 and G1, **everything**. That is the entire point of the vectors.

- **Light** builds and tests the firmware handler against the vectors from a
  laptop terminal with a Python framing script. No Prime in the loop.
- **Prime** builds and tests the sync engine against a **fake Light** — a Python
  process replaying the vectors on a PTY. No firmware in the loop.

Neither session waits on the other. Neither session needs the other's hardware.
They meet at G3, and the animation — genuinely the last thing — comes after.

### Findings from the Light codebase that change the plan

Three things surfaced from reading the firmware. They are already folded into
`DOCK-PROTOCOL.md` and into Phase 1 below, but they are recorded here because
they overturn stated assumptions:

1. **The FS layer permits exactly one open file at a time**
   (`src/core_storage.cpp`). The existing code is built around this — config
   writes *refuse* rather than steal the handle from a running capture. This
   **invalidates the original quiesce rule**: logging cannot resume into a fresh
   file while the dock serves a `GET`, because the logger would hold the only
   handle. A dock session therefore **suspends logging for its duration**
   (`DOCK-PROTOCOL.md` §6) — stated in `HELLO`'s flags, recorded in the next log
   file's header, and force-resumed by a 10 s watchdog so a yanked cable can
   never leave logging switched off. **This is the veto-class decision in this
   plan.** It is a real cost against "black-box behaviour survives docking" — but
   it is the honest version of a cost MSC would have imposed silently.

2. **The test hook doesn't merely collide with framing — it eats it.**
   `input::poll()` (`src/core_ui.cpp`) reads any available serial byte and
   *swallows* unmatched ones (`default: break`). Choosing a distinctive start
   byte is necessary but nowhere near sufficient: the dock reader must drain
   Serial **before** `input::poll()` in the main loop, and must own the port
   outright once framing is detected.

3. **`/tables/` has no consumer on Light yet.** The decode layer is not written
   (README: "Not yet implemented"). The dock provisions ahead of its consumer,
   deliberately. A successful table push with no visible effect on Light is
   **correct**, and Prime's session log should not imply otherwise.

---

## The DOCK-PROTOCOL.md contract (spec this first) — **[S] DRAFTED, needs ratification**

Mirrored verbatim into both repos, PROTOCOL.md-style. Nothing on either
side is built until this is written.

> **Status:** `DOCK-PROTOCOL.md` v0.1 is written in this repo. Every box below is
> satisfied by it. What remains is **ratification by the Prime session** and
> mirroring the file into kilodash. The checkboxes stay open until that happens.
>
> Where the draft went beyond the sketch below, and why:
>
> - **`max_payload` is negotiated in `HELLO`**, not fixed by the spec. This is
>   what demotes Phase 0 from a blocking gate to a one-function dependency.
> - **`LIST` takes a `want_hashes` flag.** Tables are kilobytes and get hashed
>   for an exact diff; logs are megabytes and don't, because hashing every log on
>   every `LIST` would cost seconds for nothing.
> - **Light hashes each log as it streams it** and caches the digest, so
>   `DELETE`'s verify is free rather than a second full read of a multi-MB file.
> - **A 10 s Light-side watchdog** force-resumes logging if the cable dies
>   mid-session. This is the only place Light acts unbidden; it exists because
>   finding #1 means an abandoned dock session would otherwise leave the black
>   box switched off.

- [ ] Framed request/response over CDC: sync-safe framing (start byte,
      length, type, payload, CRC-16) — **never** bare single characters,
      which would collide with the `SL_TEST_HOOK` serial UI driver on a
      dev build. Prime is the only initiator; Light only ever replies.
- [ ] Command set (positive allow-list, the complete surface):
      - [ ] `HELLO` → identity, firmware version, protocol version, SD
            present?, RTC epoch + "was it ever set this power cycle"
      - [ ] `SET_CLOCK(epoch, quality)` — quality ∈ {ntp, rtc, unsynced}
      - [ ] `LIST(dir)` → name/size/mtime for `/tables/`, `/logs/`
      - [ ] `PUT(path, offset, chunk)` → tables + Tier-3 `/config.json`
            only; firmware rejects writes anywhere else (allow-list of
            writable prefixes, enforced Light-side)
      - [ ] `COMMIT(path, sha256)` — Light verifies the staged tmp file's
            hash, then atomically renames. No commit, no file.
      - [ ] `GET(path, offset)` → chunked read of **closed** files under
            `/logs/` only
      - [ ] `DELETE(path)` → `/logs/` only, and only after a verified pull
            (Prime sends the sha256 it received; Light checks before
            unlinking) — this is how "pull" ≠ "copy that fills the card"
      - [ ] `BYE` — Light returns to fully standalone behavior
- [ ] Reject pass, independent of the allow-list (defense in depth): any
      path escaping the two roots, any write to `/logs/`, any read of
      `/config/` Tier-2 files, any unknown type → error frame, logged.
- [ ] Versioning rule: `HELLO` carries protocol version; mismatch degrades
      to clock-set only (the one operation every version must support).
- [ ] Table shape on Light's SD: `/tables/` flat dir, same Canboat-subset
      JSON as the Prime store — this **is** the SD-export shape already
      promised in `TABLES.md`; link the two docs, don't restate the schema.

## Gate G1 — Conformance vectors — **[S] blocks both lanes**

- [x] `dock-vectors.json` per `DOCK-PROTOCOL.md` §10: hex frames + expected
      responses for every command's happy path, every error code, and the
      framing edges — bad CRC, oversized `LEN`, garbage-before-SOF resync, and a
      bare `a` from the test hook's alphabet arriving mid-stream.
- [x] Committed to **both** repos. Light unit-tests its codec against it; Prime
      runs its engine against a fake Light that replays it.
- [x] Whichever session writes it, the other reviews it. A vector file written
      by one side alone tests one side's misreading of the spec twice.

## Phase 0 — Bench facts — **[S]; blocks only Prime's `devices.py`**

- [ ] Plug Light into Prime, `lsusb`, **write down the VID:PID** (expect
      Seeed's VID; do not hardcode from memory — record what enumerates,
      same discipline as the FX2LP bring-up).
- [ ] Confirm which `/dev/ttyACM*` it lands on **with a CanTick also
      plugged in** — two CDC devices must be distinguishable by VID +
      product string, never by ACM index.
- [ ] Measure real CDC throughput with a dumb echo sketch → sets Prime's chunk
      size and the progress-bar math for Phase 4. **No longer sets the wire
      format** — `max_payload` is negotiated in `HELLO`.
- [ ] Note for the Mac bench: `lsusb` is the Pi. On the dev Mac it's
      `system_profiler SPUSBDataType` / `ioreg -p IOUSB`. Record what
      enumerates on **Prime**, since Prime is what has to match it.

## Phase 1 — Light firmware: dock handler — **[L] this repo**

- [x] **Frame codec** as its own module: `SOF`/CRC-16-CCITT-FALSE encode +
      decode, the resync rule, bounded reads. Unit-tested against
      `dock-vectors.json` (G1) and against the spec's CRC check value —
      `"123456789"` → `0x29B1`.
- [x] Protocol handler as its own module, entered only when framed traffic
      is detected — coexists with (and in field builds, replaces) the
      test hook's raw-character reader.
- [x] **Serial ownership.** `input::poll()` swallows unmatched serial bytes
      (`core_ui.cpp`, `default: break`), so it would eat the frame stream. The
      dock reader must drain Serial **before** `input::poll()` in the main loop,
      and once framing is detected the test hook must not touch the port at all.
      A distinctive `SOF` is necessary and not sufficient.
- [x] **Quiesce rule — logging is suspended for the dock session.** *(Rewritten:
      the original "rotate and keep logging" is impossible — the FS layer allows
      one open file at a time and the logger would hold it.)* First valid frame
      closes the active log file and marks logging suspended; `HELLO` flags say
      so; `BYE` — or the **10 s watchdog** — resumes into a fresh file whose
      header records the dock gap. The watchdog is not optional: without it a
      yanked cable leaves the black box switched off.
- [x] `SET_CLOCK` sets the SAMD51 RTC and records the quality flag; the
      logger stamps subsequent files with real epoch + a "clock source"
      note in the file header so forensics knows what it's trusting.
      **Careful:** no identifier may be named `RTC` — CMSIS defines it as an
      object-like macro (see README, "Two details worth knowing").
- [x] **Vendor a software sha256** into `lib/`. The SAMD51 has no crypto
      accelerator (that's the SAML), so this is CPU work — and `COMMIT` and
      `DELETE` both depend on it. Budget it: hashing a multi-MB log is seconds,
      which is why the contract gives those two commands a 30 s timeout and why
      `GET` hashes as it streams.
- [x] Staged writes: `PUT` chunks land in `<path>.partial` on SD; `COMMIT`
      verifies sha256 then renames. Power loss mid-PUT leaves only a
      `.partial` to sweep at boot.
- [x] **Boot-time `.partial` sweep** — the thing that makes mid-`PUT` power loss
      invisible rather than cumulative. Hook it into `storage::begin()`.
- [x] `storage::ensureDir("/tables")` alongside the existing `/logs` and
      `/config` — the directory must exist before the first `PUT` lands.
- [x] **`GET` streams and hashes in one pass**, caching `(path, size, digest)` so
      `DELETE`'s verify doesn't re-read the whole file.
- [x] Writable-prefix allow-list + reject pass per the contract, with a
      unit test per rejected path class (escape, `/logs/` write, Tier-2
      read).
- [x] SD-absent behavior: `HELLO` says so; `LIST`/`PUT`/`GET` return a
      clean "no SD" error rather than timing out — Prime's log tells the
      truth ("clock synced; tables skipped — no SD in Light").

## Phase 2 — Prime: detection + dock service — **[P] kilodash**

- [ ] `devices.py`: add Light detection cloning the `cantick_tty()`
      pattern — VID + product-string match from Phase 0 → its ttyACM
      node. New device key (`scottinalight`), green live badge like the
      other hotplug tiles.
- [ ] Sync engine as a `system.Task`-driven module (`lightdock.py`),
      pure logic separated from the screen: HELLO → clock → tables →
      logs, emitting per-item state events the screen renders.
      - [ ] **Clock:** read Prime's own sync state (`timedatectl` /
            Pi 5 RTC) and send honest quality. Pi 5's RTC only holds
            through power-off with the coin cell fitted — never claim
            `ntp` unless NTP is actually synchronized right now.
      - [ ] **Tables:** diff enabled tables in `/opt/kilodash/tables/`
            (the store the Files screen and Tables tile already share)
            against Light's `LIST /tables/` by name+sha256; PUT/COMMIT
            only the missing/stale ones. Prime-always-wins = stale on
            Light gets overwritten, never merged.
      - [ ] **Logs (if toggle on):** GET closed logs into
            `/opt/kilodash/captures/` (they ride the existing Files
            screen USB-offload path for free), verify sha256, then
            DELETE on Light. Skip files already pulled (name+size+hash).
- [ ] **Resume logic is just the diff run again.** No resume state is
      persisted anywhere — redock reruns HELLO→diff and only the missing
      pieces transfer. Interruption safety comes from COMMIT atomicity,
      not bookkeeping.
- [ ] Argv/IO discipline as everywhere: no shell, bounded reads, every
      frame CRC-checked, timeouts on every request so a wedged Light
      degrades to a truthful log line, not a hung screen.

- [ ] **Fake Light** for development: a Python process replaying
      `dock-vectors.json` on a PTY. The whole sync engine is built and tested
      against it — no firmware, no hardware, no waiting on the Light lane.

## Phase 3 — Prime: Light Dock screen — **[P] kilodash**

Hotplug tile, `screens.base.Screen`, gated on the `scottinalight` device
key. Splits 480×320 roughly in half.

- [ ] **Top pane — the across-the-room animation.** Stylistics follow the
      existing splash curtain: same phosphor/CRT visual language as
      `ScottinaSplash.gif` and Light's phosphor-rain boot, drawn in the
      active theme's palette (chrome monochrome, status colours vivid —
      matching the theme rules both devices already share).
      - [ ] **Syncing:** the two device silhouettes (Prime's slab, Light's
            smaller slab) drawing together, cable between them alive with
            phosphor pulses; pulse rate maps loosely to transfer activity.
      - [ ] **Complete:** the hug — devices together, steady glow. Visible
            and unambiguous at 3 m.
      - [ ] **Interrupted:** sad-face beat + the cable visibly broken.
            Not red-alarm styling — it's "come look," not "emergency."
      - [ ] Implementation: pre-rendered frame strips or drawn primitives,
            but either way the animation region is a **dirty-rect** — it
            is the only thing repainting between log lines, per the
            KioskSpeed per-screen tick guidance. Budget it like the
            splash player does (drop a frame rather than drag).
- [ ] **Bottom pane — the session log.** The bounded timestamped list
      primitive the CAN/candump screens already use; newest at bottom;
      cleared on each dock (session-only, by decision). Every line is a
      truth about state: `clock → set (ntp)`, `tables 3/4 — pushed
      engine.json`, `logs: 12 pulled, 0 deleted (verify failed: …)`.
      On interruption the last lines *are* the incomplete-state report —
      the sad face points here.
- [ ] Controls (the only two): **Re-sync** button; **auto-pull logs**
      toggle (persisted in `config.py::DEFAULTS` so Settings renders it
      for free).
- [ ] Redock while the screen is open: detection fires, engine reruns,
      animation restarts from "syncing" — no user action.

## Phase 4 — Docs + install — **[S]; [P] owns the kilodash docs, [L] the README**

- [ ] **`docs/LIGHTDOCK.md`** — user guide in the house style of
      `docs/LANSCAN.md` / `docs/RTLSDR.md`: what docking does (clock,
      tables, logs), the two-distance screen explained (what the hug and
      the broken cable mean), the two controls, what "Prime always wins"
      means for tables, and where pulled logs land. Plain language, no
      protocol internals — those live in `DOCK-PROTOCOL.md`.
- [ ] **README.md:** new row in the hotplug device-screens table —
      **Light Dock | Scottina Light (USB) | auto-sync on dock: clock
      push, decode-table push, black-box log pull. Full user guide:
      [docs/LIGHTDOCK.md](docs/LIGHTDOCK.md)** — matching the existing
      rows' link convention exactly.
- [ ] **Scottina-Light README:** short "Docking" section pointing at
      `DOCK-PROTOCOL.md` and noting the clock-quality header now written
      into log files.
- [ ] `DOCK-PROTOCOL.md` committed to **both** repos, cross-linked, with
      the version-mismatch degradation rule stated at the top.
- [ ] No installer changes expected (pyserial is likely already a dep —
      verify; if not, add to the phase installer idempotently).

---

## Known gotchas

- **Two ttyACMs, one bench.** CanTick (Espressif VID) and Light (Seeed
  VID) will both be ttyACM*; anything keyed on ACM index will eventually
  open the wrong device and feed SLCAN bytes to a Wio Terminal. VID +
  product string, always.
- **The test hook is a protocol collision.** `SL_TEST_HOOK=1` builds treat
  single serial bytes as button presses — the framed protocol's start
  byte must be unmistakable, and the firmware handler must own the port
  once framing is detected. Field builds drop the hook entirely (already
  documented in Light's README).
- **Prime's clock isn't automatically trustworthy.** Pi 5 RTC without the
  coin cell + no network = Prime can be as lost as Light. The quality
  flag exists so a bad clock is *labeled*, never laundered into Light's
  logs as truth.
- **Delete-after-pull is the scary step.** Only unlink on Light after
  Prime confirms the received hash. A pull that verifies nothing is a
  copy; a delete that verifies nothing is data loss on a black box.
- **Don't stream, don't fuse — yet.** Live IMU/CAN streaming over the dock
  was considered and deliberately deferred: standalone logging + clock
  sync covers the forensic case. If real-time fusion ever earns its keep,
  it's a new protocol version, not a bolt-on.
- **FAT on the Light side has no journal.** Atomic rename is the strongest
  primitive available; the boot-time `.partial` sweep is what makes
  mid-PUT power loss invisible rather than cumulative.

- **The single file handle is the sharpest edge on the Light side.** One open
  file at a time, enforced by the FS layer. It is why logging suspends during a
  dock, why `GET` hashes as it streams, and why the watchdog exists. Every
  firmware design question should be checked against it first.

## Running order

**Done:** `DOCK-PROTOCOL.md` v0.1 (G0, drafted — needs Prime's ratification).

**Next, in parallel once G1 lands:**

| | Light session | Prime session |
|---|---|---|
| **now** | — | ratify `DOCK-PROTOCOL.md`, mirror it into kilodash |
| **G1** | *(one session writes `dock-vectors.json`, the other reviews)* ||
| **then** | Phase 1 — frame codec → handler → SD ops | Phase 0 bench facts → Phase 2 detection + engine vs. the fake Light |
| **then** | Light README "Docking" section | Phase 3 — the screen, animation last |
| **G3** | *(integrate on real hardware)* ||

The original instinct still holds: the contract is paper, the bench work is an
afternoon, and the firmware handler is testable from a laptop terminal with no
Prime involved. What's new is that **Prime no longer has to wait for the
firmware either** — the vectors and the fake Light cut the dependency, so both
lanes run flat out from G1.

The animation is genuinely the final polish. The feature is *done* when the log
lines are true; the hug is what makes it Scottina.
