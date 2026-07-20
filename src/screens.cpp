#include "scottina_light.h"

#include "logger_tile.h"

#include "SD/Seeed_SD.h"
#include "Seeed_FS.h"

// Button convention across every screen:
//   C     -> back
//   A     -> primary action
//   B     -> secondary action (theme cycle on the launcher)
//   5-way -> navigate / confirm

namespace {

// Which palette slot a tile's pictogram takes, mirroring the mother's
// `tile_color_key` class attribute.
enum class Ink : uint8_t { Accent, Ok, Warn, Bad, Muted };

uint16_t inkOf(Ink k) {
  const auto &p = theme::c();
  // Sterile skin: chrome is greyscale, only ok/warn/bad carry colour.
  if (p.sterile && (k == Ink::Accent || k == Ink::Muted)) return p.muted;
  switch (k) {
  case Ink::Ok: return p.ok;
  case Ink::Warn: return p.warn;
  case Ink::Bad: return p.bad;
  case Ink::Muted: return p.muted;
  default: return p.accent;
  }
}

} // namespace

// ===========================================================================
// I2C scanner  (TODO §2 + the §0 collision pick-list)
// ===========================================================================

class I2cScreen : public Screen {
public:
  const char *title() const override { return "I2C Scan"; }

  void enter() override {
    m_pickMode = false;
    m_sel = 0;
    rescan();
  }

  void onButton(Btn b) override {
    if (m_pickMode) {
      switch (b) {
      case Btn::Up: if (m_pick > 0) m_pick--; renderPick(); break;
      case Btn::Down: if (m_pick + 1 < m_candN) m_pick++; renderPick(); break;
      case Btn::Press: confirmPick(); break;
      case Btn::C: m_pickMode = false; render(); break;
      default: break;
      }
      return;
    }
    switch (b) {
    case Btn::C: ui::pop(); break;
    case Btn::A: rescan(); break;
    case Btn::Up: if (m_sel > 0) m_sel--; render(); break;
    case Btn::Down: if (m_sel + 1 < inventory::count) m_sel++; render(); break;
    case Btn::Press: openPick(); break;
    default: break;
    }
  }

private:
  bool m_pickMode = false;
  uint8_t m_sel = 0;
  uint8_t m_pick = 0;
  uint8_t m_candN = 0;
  const i2cbus::KnownDev *m_cand[8];

  void rescan() {
    ui::header("I2C Scan", "C:back  A:rescan  PRESS:identify");
    ui::clearBody();
    ui::note(0, "scanning...", theme::c().warn);
    inventory::rescan();
    Serial.print("[i2c] devices=");
    Serial.println(inventory::count);
    for (uint8_t i = 0; i < inventory::count; ++i) {
      const Detected &d = inventory::devs[i];
      Serial.print("      ");
      Serial.print(d.busName);
      Serial.print(" 0x");
      if (d.addr < 0x10) Serial.print('0');
      Serial.print(d.addr, HEX);
      Serial.print(' ');
      Serial.print(d.name);
      if (d.ambiguous) Serial.print("  [ambiguous]");
      Serial.println();
    }
    if (m_sel >= inventory::count) m_sel = 0;
    render();
  }

  void render() {
    const auto &p = theme::c();
    ui::header("I2C Scan", "C:back  A:rescan  PRESS:identify");
    ui::clearBody();
    if (inventory::count == 0) {
      ui::note(0, "no devices on either bus", p.warn);
      ui::note(2, "grove bus is empty; the internal bus", p.muted);
      ui::note(3, "carries the onboard accelerometer", p.muted);
      return;
    }
    char line[64];
    for (uint8_t i = 0; i < inventory::count && i < 11; ++i) {
      const Detected &d = inventory::devs[i];
      snprintf(line, sizeof(line), "%-8s 0x%02X  %s%s", d.busName, d.addr,
               d.name, d.ambiguous ? "  ?" : "");
      ui::row(i, line, d.ambiguous ? p.warn : p.ok, i == m_sel);
    }
  }

  void openPick() {
    if (m_sel >= inventory::count) return;
    const Detected &d = inventory::devs[m_sel];
    m_candN = i2cbus::candidatesFor(d.addr, m_cand, 8);
    if (m_candN <= 1) {
      ui::note(12, "address is unambiguous", theme::c().muted);
      return;
    }
    if (logger::isOpen()) {
      ui::note(12, "stop the capture first (SD single handle)", theme::c().warn);
      return;
    }
    m_pickMode = true;
    m_pick = 0;
    renderPick();
  }

  void renderPick() {
    const Detected &d = inventory::devs[m_sel];
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "0x%02X -- which?", d.addr);
    ui::header(hdr, "C:cancel  PRESS:confirm");
    ui::clearBody();
    for (uint8_t i = 0; i < m_candN; ++i) {
      ui::row(i, m_cand[i]->name, theme::c().fg, i == m_pick);
    }
  }

  void confirmPick() {
    Detected &d = inventory::devs[m_sel];
    const i2cbus::KnownDev *k = m_cand[m_pick];
    const bool ok = config::saveChoice(d.addr, k->name);
    d.name = k->name;
    d.cap = k->cap;
    d.ambiguous = false;
    m_pickMode = false;
    Serial.print("[i2c] user picked 0x");
    Serial.print(d.addr, HEX);
    Serial.print(" = ");
    Serial.print(k->name);
    Serial.println(ok ? " (persisted)" : " (NOT persisted -- no SD)");
    refreshAvailability();
    render();
    if (!ok) ui::note(12, "not persisted: no SD card", theme::c().warn);
  }
};

// Vibration used to live here as a bespoke screen. It is now one of three
// sensor loggers sharing a single pathway -- src/sensor_screens.cpp, built on
// include/logger_tile.h. Adding a fourth logger belongs there, not here.

// ===========================================================================
// Serial / UART autobaud + dump  (TODO §2)
// ===========================================================================

class UartScreen : public Screen {
public:
  const char *title() const override { return "Serial"; }

  void enter() override {
    ui::header("Serial", "C:back  A:restart  PRESS:pick baud");
    ui::clearBody();
    if (config::t3.uartBaud != 0) {
      lock(config::t3.uartBaud, "from config.json");
    } else {
      startProbe();
    }
  }

  void exit() override {
    Serial1.end();
    if (logger::isOpen()) logger::close();
  }

  void tick(uint32_t now) override {
    switch (m_phase) {
    case Probing: probeTick(now); break;
    case Locked: dumpTick(); break;
    default: break;
    }
  }

  void onButton(Btn b) override {
    switch (b) {
    case Btn::C: ui::pop(); break;
    case Btn::A: Serial1.end(); startProbe(); break;
    case Btn::Press:
      if (m_phase == Manual) {
        lock(BAUDS[m_manualSel], "manual");
      } else {
        m_phase = Manual;
        m_manualSel = 0;
        renderManual();
      }
      break;
    case Btn::Up:
      if (m_phase == Manual && m_manualSel > 0) { m_manualSel--; renderManual(); }
      break;
    case Btn::Down:
      if (m_phase == Manual && m_manualSel + 1 < BAUD_N) { m_manualSel++; renderManual(); }
      break;
    default: break;
    }
  }

private:
  enum Phase { Probing, Locked, Manual, NoTraffic };

  // Marine-first ordering: NMEA 0183 is 4800, AIS is 38400.
  static const uint32_t BAUDS[];
  static const uint8_t BAUD_N;
  static constexpr uint32_t WINDOW_MS = 400;

  Phase m_phase = Probing;
  uint8_t m_idx = 0;
  uint8_t m_manualSel = 0;
  uint32_t m_windowEnd = 0;
  uint16_t m_seen = 0, m_plausible = 0;
  char m_dump[3][40];

  void startProbe() {
    m_phase = Probing;
    m_idx = 0;
    ui::clearBody();
    ui::note(0, "autobaud: listening...", theme::c().warn);
    beginWindow(millis());
  }

  void beginWindow(uint32_t now) {
    Serial1.end();
    Serial1.begin(BAUDS[m_idx]);
    m_seen = 0;
    m_plausible = 0;
    m_windowEnd = now + WINDOW_MS;
    char buf[32];
    snprintf(buf, sizeof(buf), "trying %lu baud", (unsigned long)BAUDS[m_idx]);
    ui::note(1, buf, theme::c().muted);
  }

  void probeTick(uint32_t now) {
    while (Serial1.available()) {
      const int ch = Serial1.read();
      m_seen++;
      if ((ch >= 0x20 && ch <= 0x7E) || ch == '\r' || ch == '\n' || ch == '\t') {
        m_plausible++;
      }
    }
    if (now < m_windowEnd) return;

    // Framing is "clean" when nearly every byte lands in the printable set.
    if (m_seen >= 8 && m_plausible * 10 >= m_seen * 9) {
      lock(BAUDS[m_idx], "autobaud");
      return;
    }
    m_idx++;
    if (m_idx >= BAUD_N) {
      Serial1.end();
      m_phase = NoTraffic;
      ui::clearBody();
      ui::note(0, "no clean framing found", theme::c().warn);
      ui::note(1, "PRESS to pick a baud by hand", theme::c().muted);
      Serial.println("[uart] autobaud failed -> manual pick");
      return;
    }
    beginWindow(now);
  }

  void lock(uint32_t baud, const char *how) {
    Serial1.end();
    Serial1.begin(baud);
    m_phase = Locked;
    for (int i = 0; i < 3; ++i) m_dump[i][0] = '\0';
    ui::clearBody();
    char buf[48];
    snprintf(buf, sizeof(buf), "locked %lu baud (%s)", (unsigned long)baud, how);
    ui::note(0, buf, theme::c().ok);
    Serial.print("[uart] ");
    Serial.println(buf);
  }

  void dumpTick() {
    if (!Serial1.available()) return;
    char line[40];
    int n = 0;
    while (Serial1.available() && n < 24) {
      const int ch = Serial1.read();
      line[n++] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.';
    }
    line[n] = '\0';
    if (logger::isOpen()) logger::writeLine(line);
    strncpy(m_dump[0], m_dump[1], sizeof(m_dump[0]));
    strncpy(m_dump[1], m_dump[2], sizeof(m_dump[1]));
    strncpy(m_dump[2], line, sizeof(m_dump[2]) - 1);
    m_dump[2][sizeof(m_dump[2]) - 1] = '\0';
    for (int i = 0; i < 3; ++i) ui::note(2 + i, m_dump[i], theme::c().fg);
  }

  void renderManual() {
    ui::header("Serial -- baud", "C:back  PRESS:lock");
    ui::clearBody();
    char buf[16];
    for (uint8_t i = 0; i < BAUD_N; ++i) {
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)BAUDS[i]);
      ui::row(i, buf, theme::c().fg, i == m_manualSel);
    }
  }
};

const uint32_t UartScreen::BAUDS[] = {4800, 9600, 38400,  19200,
                                      57600, 115200, 2400, 31250};
const uint8_t UartScreen::BAUD_N = sizeof(UartScreen::BAUDS) / sizeof(uint32_t);

// ===========================================================================
// CAN  (TODO §4 -- presence only; sniff/decode land on the next pass)
// ===========================================================================

class CanScreen : public Screen {
public:
  const char *title() const override { return "CAN Bus"; }

  void enter() override {
    ui::header("CAN Bus", "C:back  A:re-probe");
    ui::clearBody();
    probe();
  }

  void onButton(Btn b) override {
    switch (b) {
    case Btn::C: ui::pop(); break;
    case Btn::A: probe(); break;
    default: break;
    }
  }

private:
  void probe() {
    const auto &p = theme::c();
    ui::clearBody();
    const uint8_t cs = config::t3.mcp2515CsPin;
    const bool present = canfe::detect(cs);

    char buf[48];
    snprintf(buf, sizeof(buf), "MCP2515 on CS pin %u: %s", cs,
             present ? "PRESENT" : "absent");
    ui::note(0, buf, present ? p.ok : p.warn);
    Serial.print("[can] ");
    Serial.println(buf);

    if (!present) {
      ui::note(2, "Attach the Grove MCP2515 to the header", p.muted);
      ui::note(3, "SPI, then press A to re-probe.", p.muted);
      return;
    }
    ui::note(2, "Controller answers in config mode.", p.fg);
    ui::note(4, "Bitrate autobaud + passive sniff are", p.muted);
    ui::note(5, "not implemented yet (TODO 4/5).", p.muted);
  }
};

// ===========================================================================
// Logs  (TODO §3)
// ===========================================================================

class LogBrowserScreen : public Screen {
public:
  const char *title() const override { return "Logs"; }

  void enter() override {
    ui::header("Logs", "C:back  A:refresh");
    ui::clearBody();
    list();
  }

  void onButton(Btn b) override {
    switch (b) {
    case Btn::C: ui::pop(); break;
    case Btn::A: list(); break;
    case Btn::Up: if (m_sel > 0) m_sel--; render(); break;
    case Btn::Down: if (m_sel + 1 < m_n) m_sel++; render(); break;
    default: break;
    }
  }

private:
  static constexpr uint8_t MAXF = 10;
  char m_names[MAXF][32];
  uint32_t m_sizes[MAXF];
  uint8_t m_n = 0;
  uint8_t m_sel = 0;

  void list() {
    m_n = 0;
    if (!storage::mounted()) {
      ui::clearBody();
      ui::note(0, "no SD card", theme::c().warn);
      return;
    }
    if (logger::isOpen()) {
      ui::clearBody();
      ui::note(0, "capture in progress", theme::c().warn);
      ui::note(1, "stop it before browsing (single handle)", theme::c().muted);
      return;
    }

    File dir = SD.open("/logs");
    if (dir) {
      File e = dir.openNextFile();
      while (e && m_n < MAXF) {
        if (!e.isDirectory()) {
          strncpy(m_names[m_n], e.name(), sizeof(m_names[0]) - 1);
          m_names[m_n][sizeof(m_names[0]) - 1] = '\0';
          m_sizes[m_n] = e.size();
          m_n++;
        }
        e.close();
        e = dir.openNextFile();
      }
      dir.close();
    }
    if (m_sel >= m_n) m_sel = 0;
    render();
  }

  void render() {
    const auto &p = theme::c();
    ui::clearBody();
    char line[64];
    if (m_n == 0) {
      ui::note(0, "no captures yet", p.muted);
    } else {
      for (uint8_t i = 0; i < m_n; ++i) {
        snprintf(line, sizeof(line), "%-22s %6lu B", m_names[i],
                 (unsigned long)m_sizes[i]);
        ui::row(i, line, p.fg, i == m_sel);
      }
    }
    snprintf(line, sizeof(line), "SD %lu MB free of %lu MB",
             (unsigned long)storage::freeMB(), (unsigned long)storage::totalMB());
    ui::note(11, line, p.accent);
  }
};

// ===========================================================================
// Launcher  (mother: kilodash/screens/home.py -- adaptive tile grid)
// ===========================================================================

namespace {
I2cScreen s_i2c;
UartScreen s_uart;
CanScreen s_can;
LogBrowserScreen s_logs;

bool availI2c() { return true; }
bool availImu() { return inventory::hasCap(i2cbus::Cap::Accel); }
bool availUart() { return true; }
bool availCan() { return canfe::present(); }
bool availLogs() { return storage::mounted(); }
// Mic and light sensor are soldered to the board; there is nothing to hot-plug
// and nothing to detect, so these two are simply always there.
bool availOnboard() { return true; }

struct Tile {
  const char *label;
  // An accessor rather than a Screen*, so the table stays a genuine link-time
  // constant even for screens that live in another translation unit.
  Screen *(*screen)();
  pict::Glyph glyph;
  Ink ink;
  bool (*available)();
  // The live badge, and it means exactly ONE thing: this hardware can be
  // UNPLUGGED. The SD card lifts out; the Grove MCP2515 comes off the header.
  //
  // It is NOT "was detected at runtime". The LIS3DHTR is discovered by an I2C
  // scan rather than read from a fixed ADC pin, and it wore a badge on that
  // basis alone -- but it is soldered to the board, exactly as permanent as the
  // mic and the light sensor beside it. That badge described how the firmware
  // found the part, not anything the operator could act on, so it is gone.
  //
  // Availability is a separate mechanism: `available` still hides a tile whose
  // hardware does not answer, badge or no badge.
  bool device;
};

// Order and colour keys follow the mother's screen registry where a subsystem
// exists on both.
//
// The three sensor loggers sit together and are deliberately identical in
// shape, size and weight -- same tile geometry, same glyph radius, same card,
// and now no odd badge on one of them. They are one instrument with three
// soldered-in inputs, and the grid should say so.
const Tile TILES[] = {
    {"I2C Scan", i2cScreen, pict::Glyph::I2c, Ink::Ok, availI2c, false},
    {"Vibration", vibrationScreen, pict::Glyph::Vibration, Ink::Warn, availImu, false},
    {"Sound", soundScreen, pict::Glyph::Sound, Ink::Accent, availOnboard, false},
    {"Light", lightScreen, pict::Glyph::Light, Ink::Ok, availOnboard, false},
    {"Serial", uartScreen, pict::Glyph::Serial, Ink::Muted, availUart, false},
    {"CAN Bus", canScreen, pict::Glyph::Can, Ink::Bad, availCan, true},
    {"Logs", logBrowserScreen, pict::Glyph::Logs, Ink::Accent, availLogs, true},
};
constexpr uint8_t TILE_N = sizeof(TILES) / sizeof(TILES[0]);

constexpr int MARGIN = 10;
constexpr int GAP = 8;
constexpr int COLS = 2;
constexpr int RADIUS = 10;
constexpr int TILE_H_MAX = 62;
constexpr uint32_t ROTATE_MS = 3000;
} // namespace

class LauncherScreen : public Screen {
public:
  const char *title() const override { return SL_PRODUCT; }

  void enter() override {
    rebuild();
    render();
    m_nextTick = millis() + 1000;
  }

  void tick(uint32_t now) override {
    if (now < m_nextTick) return;
    m_nextTick = now + 1000;

    // Hot-plug reconciliation. Safe here because the launcher is the only screen
    // running: re-probing the MCP2515 issues a controller RESET, which must
    // never happen underneath an active capture.
    storage::refresh();
    canfe::detect(config::t3.mcp2515CsPin);
    if (++m_scanDivider >= 3) {
      m_scanDivider = 0;
      inventory::rescan();
    }

    const uint8_t before = m_n;
    rebuild();
    if (m_n != before) {
      render(); // the tile set changed -- lay the grid out again
    } else {
      drawHeader(); // vitals + uptime only
    }
  }

  void onButton(Btn b) override {
    switch (b) {
    case Btn::Up:
      if (m_sel >= COLS) { m_sel -= COLS; render(); }
      break;
    case Btn::Down:
      if (m_sel + COLS < m_n) { m_sel += COLS; render(); }
      break;
    case Btn::Left:
      if (m_sel > 0) { m_sel--; render(); }
      break;
    case Btn::Right:
      if (m_sel + 1 < m_n) { m_sel++; render(); }
      break;
    case Btn::Press:
    case Btn::A:
      if (m_n) ui::push(m_visible[m_sel]->screen());
      break;
    case Btn::B: {
      theme::cycle();
      config::saveTheme(theme::c().name);
      Serial.print("[ui ] theme=");
      Serial.println(theme::c().name);
      ui::tft.fillScreen(theme::c().bg);
      render();
      break;
    }
    default: break;
    }
  }

private:
  const Tile *m_visible[TILE_N];
  uint8_t m_n = 0;
  uint8_t m_sel = 0;
  uint8_t m_scanDivider = 0;
  uint32_t m_nextTick = 0;

  void rebuild() {
    m_n = 0;
    for (uint8_t i = 0; i < TILE_N; ++i) {
      if (TILES[i].available()) m_visible[m_n++] = &TILES[i];
    }
    if (m_sel >= m_n) m_sel = m_n ? m_n - 1 : 0;
  }

  // Rotating vitals in the header, standing in for the mother's rotating
  // WiFi/LAN IP -- this instrument has no network to report.
  void drawHeader() {
    const auto &p = theme::c();
    const int w = ui::tft.width();
    ui::tft.fillRect(0, 0, w, ui::HEADER_H, p.card);

    char label[6], value[20];
    const uint8_t slot = (uint8_t)((millis() / ROTATE_MS) % 3);
    switch (slot) {
    case 0:
      strcpy(label, "I2C");
      snprintf(value, sizeof(value), "%u dev", inventory::count);
      break;
    case 1:
      strcpy(label, "SD");
      if (storage::mounted()) {
        snprintf(value, sizeof(value), "%lu MB free",
                 (unsigned long)storage::freeMB());
      } else {
        strcpy(value, "no card");
      }
      break;
    default:
      strcpy(label, "CAN");
      strcpy(value, canfe::present() ? "MCP2515" : "absent");
      break;
    }

    ui::tft.setTextDatum(TL_DATUM);
    ui::tft.setTextFont(1);
    ui::tft.setTextColor(p.muted, p.card);
    ui::tft.drawString(label, 12, 3);
    ui::tft.setTextFont(2);
    ui::tft.setTextColor(p.accent, p.card);
    ui::tft.drawString(value, 12, 14);

    // Page dots for the rotation, as on the mother's launcher header.
    for (int i = 0; i < 3; ++i) {
      ui::tft.fillCircle(w - 46 + i * 9, 28, 2, i == slot ? p.accent : p.card_hi);
    }

    // No RTC on this board, so uptime takes the clock's place.
    const uint32_t s = millis() / 1000;
    char up[10];
    snprintf(up, sizeof(up), "%lu:%02lu", (unsigned long)(s / 60),
             (unsigned long)(s % 60));
    ui::tft.setTextFont(2);
    ui::tft.setTextColor(p.fg, p.card);
    ui::tft.setTextDatum(TR_DATUM);
    ui::tft.drawString(up, w - 10, 4);
    ui::tft.setTextDatum(TL_DATUM);
  }

  void render() {
    const auto &p = theme::c();
    drawHeader();
    ui::tft.fillRect(0, ui::HEADER_H, ui::tft.width(),
                     ui::tft.height() - ui::HEADER_H, p.bg);

    if (m_n == 0) {
      ui::note(2, "no subsystems available", p.warn);
      return;
    }

    const int w = ui::tft.width();
    const int top = ui::HEADER_H + 6;
    const int rows = (m_n + COLS - 1) / COLS;
    const int avail = ui::tft.height() - top - 6;
    int th = (avail - (rows - 1) * GAP) / rows;
    if (th > TILE_H_MAX) th = TILE_H_MAX;
    const int tw = (w - MARGIN * 2 - GAP * (COLS - 1)) / COLS;

    for (uint8_t i = 0; i < m_n; ++i) {
      const int r = i / COLS, c = i % COLS;
      const int x0 = MARGIN + c * (tw + GAP);
      const int y0 = top + r * (th + GAP);
      drawTile(*m_visible[i], x0, y0, tw, th, i == m_sel);
    }

    ui::tft.setTextFont(1);
    ui::tft.setTextColor(p.muted, p.bg);
    ui::tft.setTextDatum(TL_DATUM);
    ui::tft.drawString("5-way:move  PRESS:open  B:theme", 6,
                       ui::tft.height() - ui::FOOTER_H + 3);
  }

  void drawTile(const Tile &t, int x0, int y0, int tw, int th, bool selected) {
    const auto &p = theme::c();
    const uint16_t surface = selected ? p.card_hi : p.card;
    ui::tft.fillRoundRect(x0, y0, tw, th, RADIUS, surface);
    if (selected) {
      ui::tft.drawRoundRect(x0, y0, tw, th, RADIUS, p.accent);
      ui::tft.drawRoundRect(x0 + 1, y0 + 1, tw - 2, th - 2, RADIUS - 1, p.accent);
    }

    const int cx = x0 + tw / 2;
    const int gy = y0 + (int)(th * 0.32f);
    const int gr = (int)(th * 0.21f);
    pict::draw(t.glyph, cx, gy, gr, inkOf(t.ink), surface);

    if (t.device) {
      ui::tft.fillSmoothCircle(x0 + tw - 14, y0 + 12, 4, p.ok, surface);
    }

    // Prefer the 16px face; drop to the small font when a label would overrun.
    uint8_t font = 2;
    if (ui::tft.textWidth(t.label, 2) > tw - 10) font = 1;
    ui::tft.setTextFont(font);
    ui::tft.setTextColor(p.fg, surface);
    ui::tft.setTextDatum(TC_DATUM);
    ui::tft.drawString(t.label, cx, y0 + (int)(th * 0.56f));
    ui::tft.setTextDatum(TL_DATUM);
  }
};

namespace {
LauncherScreen s_launcher;
}

Screen *launcherScreen() { return &s_launcher; }
Screen *i2cScreen() { return &s_i2c; }
Screen *uartScreen() { return &s_uart; }
Screen *canScreen() { return &s_can; }
Screen *logBrowserScreen() { return &s_logs; }

void refreshAvailability() { canfe::detect(config::t3.mcp2515CsPin); }

void screensInit() {
  refreshAvailability();
  // Sound calibration is loaded at boot and SURVIVES a reboot, unlike the
  // band thresholds which deliberately do not. Cal is painful to redo; a
  // threshold is four seconds with a thumb switch (§4).
  soundcal::load();
}
