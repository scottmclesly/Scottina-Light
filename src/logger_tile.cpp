#include "logger_tile.h"

#include <ArduinoJson.h>
#include <math.h>

#include "SD/Seeed_SD.h"
#include "Seeed_FS.h"

// ===========================================================================
// Buzzer  (§3)
// ===========================================================================

namespace buzzer {

namespace {
const uint16_t *s_seq = nullptr;
uint8_t s_n = 0;
uint8_t s_i = 0;
uint32_t s_nextMs = 0;
bool s_begun = false;

void drive(bool on) { analogWrite(WIO_BUZZER, on ? 160 : 0); }
} // namespace

void begin() {
  if (s_begun) return;
  pinMode(WIO_BUZZER, OUTPUT);
  drive(false);
  s_begun = true;
}

void pattern(const uint16_t *seq, uint8_t n) {
  if (!s_begun || !seq || n == 0) return;
  s_seq = seq;
  s_n = n;
  s_i = 0;
  drive(true);
  s_nextMs = millis() + seq[0];
}

void stop() {
  s_seq = nullptr;
  s_n = 0;
  if (s_begun) drive(false);
}

void tick(uint32_t nowMs) {
  if (!s_seq) return;
  if ((int32_t)(nowMs - s_nextMs) < 0) return;
  s_i++;
  if (s_i >= s_n) {
    stop();
    return;
  }
  drive((s_i & 1) == 0); // even index = ON, odd = OFF
  s_nextMs = nowMs + s_seq[s_i];
}

} // namespace buzzer

namespace {
// Three short pips: distinct from anything else this instrument does.
const uint16_t TRIP_SEQ[] = {70, 60, 70, 60, 70};
// While latched, one pip every few seconds -- an alert you can walk away from
// and still be told about.
const uint16_t CHIRP_SEQ[] = {50};
constexpr uint32_t CHIRP_PERIOD_MS = 5000;
constexpr uint32_t FLASH_MS = 250;
constexpr uint32_t VALUE_MS = 200; // card value repaint, decimated from sampling
} // namespace

// ===========================================================================
// Sound reference calibration  (§4)
// ===========================================================================

namespace soundcal {

namespace {
Record s_rec = {0.0f, 0.0f, 0.0f, 0, false};
constexpr const char *CAL_PATH = "/cal/sound.json";
constexpr const char *CAL_PART = "/cal/sound.json.partial";
} // namespace

const Record &get() { return s_rec; }
bool valid() { return s_rec.valid; }

bool load() {
  s_rec.valid = false;
  if (!storage::mounted() || !SD.exists(CAL_PATH)) return false;

  File f = SD.open(CAL_PATH, FILE_READ);
  if (!f) return false;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();

  // Every failure below lands in the same place: relative, not fake dB (§4).
  if (err) {
    Serial.print("[cal] sound.json malformed -> staying relative: ");
    Serial.println(err.c_str());
    return false;
  }
  if (!doc["ref_db"].is<float>() || !doc["ref_dbfs"].is<float>()) {
    Serial.println("[cal] sound.json incomplete -> staying relative");
    return false;
  }

  const float refDb = doc["ref_db"].as<float>();
  const float refDbfs = doc["ref_dbfs"].as<float>();
  const float offset = refDb - refDbfs;
  if (!isfinite(refDb) || !isfinite(refDbfs) || refDb < 20.0f || refDb > 160.0f) {
    Serial.println("[cal] sound.json out of range -> staying relative");
    return false;
  }

  s_rec.refDb = refDb;
  s_rec.refDbfs = refDbfs;
  s_rec.offset = offset;
  s_rec.captured = doc["captured"] | (uint64_t)0;
  s_rec.valid = true;
  Serial.print("[cal] sound offset=");
  Serial.print(offset, 1);
  Serial.println(" dB loaded");
  return true;
}

bool save(float refDb, float refDbfs) {
  if (!storage::mounted()) return false;
  // Refuse rather than steal the single file handle from a running capture,
  // the same rule config writes follow.
  if (logger::isOpen()) return false;
  storage::ensureDir("/cal");

  JsonDocument doc;
  doc["ref_db"] = refDb;
  doc["ref_dbfs"] = refDbfs;
  doc["offset"] = refDb - refDbfs;
  doc["captured"] = wallclock::now();
  doc["clock"] = wallclock::qualityName();

  // tmp+rename, per the shared-data convention: a half-written cal record must
  // never be readable as a whole one.
  SD.remove(CAL_PART);
  File f = SD.open(CAL_PART, FILE_WRITE);
  if (!f) return false;
  const bool wrote = serializeJson(doc, f) > 0;
  f.close();
  if (!wrote) {
    SD.remove(CAL_PART);
    return false;
  }
  SD.remove(CAL_PATH);
  if (!SD.rename(CAL_PART, CAL_PATH)) {
    SD.remove(CAL_PART);
    return false;
  }

  s_rec.refDb = refDb;
  s_rec.refDbfs = refDbfs;
  s_rec.offset = refDb - refDbfs;
  s_rec.captured = wallclock::now();
  s_rec.valid = true;
  Serial.print("[cal] sound calibrated: offset=");
  Serial.print(s_rec.offset, 1);
  Serial.println(" dB");
  return true;
}

} // namespace soundcal

// ===========================================================================
// LoggerScreen
// ===========================================================================

// --- axis mapping ----------------------------------------------------------

int LoggerScreen::yOf(float v) const {
  const float span = m_spec.axisMax - m_spec.axisMin;
  if (span <= 0.0f) return GH - 1;
  float t = (m_spec.axisMax - v) / span;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return (int)lroundf(t * (float)(GH - 1));
}

float LoggerScreen::valOf(int y) const {
  const float span = m_spec.axisMax - m_spec.axisMin;
  return m_spec.axisMax - ((float)y / (float)(GH - 1)) * span;
}

void LoggerScreen::fmt(float v, char *out, size_t n) const {
  snprintf(out, n, "%.*f", (int)m_spec.decimals, (double)v);
}

// --- drawing ---------------------------------------------------------------

void LoggerScreen::drawFrame() {
  const auto &p = theme::c();
  // The latch flashes the graph frame. Badge + flash, never a modal (§3).
  const uint16_t col = (m_latched && m_flashOn) ? p.bad : p.muted;
  ui::tft.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, col);
}

// A parked marker is drawn visibly "off" -- muted, not warn (§1).
uint16_t LoggerScreen::hiColour() const {
  const auto &p = theme::c();
  if (m_grab == Grab::High) return p.bad;
  return hiArmed() ? p.warn : p.muted;
}

uint16_t LoggerScreen::loColour() const {
  const auto &p = theme::c();
  if (m_grab == Grab::Low) return p.bad;
  return loArmed() ? p.warn : p.muted;
}

// Markers and cursor are dashed 2-on/2-off on OPPOSITE phases, so a cursor
// resting on a marker still reads as two distinct lines rather than one.
void LoggerScreen::overlayColumn(int x) {
  const int sx = GX + x;
  if (((x >> 1) & 1) == 0) {
    ui::tft.drawPixel(sx, GY + m_hiY, hiColour());
    ui::tft.drawPixel(sx, GY + m_loY, loColour());
  } else if (m_grab == Grab::None) {
    // While grabbed the cursor IS the marker, so it is not drawn twice.
    ui::tft.drawPixel(sx, GY + m_curY, theme::c().accent);
  }
}

// The same dash pattern as a run of short spans instead of 308 single pixels.
// This is the difference between a full repaint the cursor can keep up with and
// one it cannot: marker drawing drops from ~900 pixel ops to ~230 span ops.
void LoggerScreen::drawMarkerLine(int y, uint16_t col, int phase) {
  for (int x = phase; x < GW; x += 4) {
    const int w = (x + 2 > GW) ? GW - x : 2;
    ui::tft.drawFastHLine(GX + x, GY + y, w, col);
  }
}

void LoggerScreen::drawTraceColumn(int x) {
  const auto &p = theme::c();
  const int sx = GX + x;
  ui::tft.drawFastVLine(sx, GY, GH, p.bg);

  const uint8_t v = m_col[x];
  if (v != EMPTY) {
    const int y = (int)v;
    // Dim fill under a bright cap: the scope look, and it keeps the threshold
    // lines legible where a solid bar would swallow them.
    if (y < GH - 1) ui::tft.drawFastVLine(sx, GY + y + 1, GH - 1 - y, p.card_hi);
    ui::tft.drawFastVLine(sx, GY + y, (y <= GH - 2) ? 2 : 1, p.ok);
  }
}

void LoggerScreen::drawColumn(int x) {
  drawTraceColumn(x);
  overlayColumn(x);
}

void LoggerScreen::redrawGraph() {
  for (int x = 0; x < GW; ++x) drawTraceColumn(x);
  drawMarkerLine(m_hiY, hiColour(), 0);
  drawMarkerLine(m_loY, loColour(), 0);
  if (m_grab == Grab::None) drawMarkerLine(m_curY, theme::c().accent, 2);
}

void LoggerScreen::drawValue() {
  const auto &p = theme::c();
  char vb[24];
  if (m_have) {
    // Fixed-width field drawn with an opaque background: no fillRect, so no
    // flicker at the sample rate.
    snprintf(vb, sizeof(vb), "%7.*f", (int)m_spec.decimals, (double)m_last);
  } else {
    snprintf(vb, sizeof(vb), "%7s", "--");
  }
  ui::tft.setTextFont(4);
  ui::tft.setTextDatum(TL_DATUM);
  ui::tft.setTextColor(m_latched ? p.bad : p.fg, p.card);
  ui::tft.drawString(vb, CX + 8, CY + 16);
}

void LoggerScreen::drawCard() {
  const auto &p = theme::c();
  ui::tft.fillRoundRect(CX, CY, CW, CH, 8, p.card);

  char line[72], vb[24];

  // Label line, in priority order: the marker you are holding, then the spike
  // that tripped the latch, then the resting metric plus the cursor value.
  uint16_t labelCol = p.muted;
  if (m_grab != Grab::None) {
    fmt(valOf(m_curY), vb, sizeof(vb));
    snprintf(line, sizeof(line), "HOLDING %s  %s %s",
             m_grab == Grab::High ? "HIGH" : "LOW", vb, m_spec.units);
    labelCol = p.bad;
  } else if (m_latched) {
    // The value that tripped it, held on screen after the signal has walked
    // back inside the band. This is the number you came back to read.
    fmt(m_tripValue, vb, sizeof(vb));
    snprintf(line, sizeof(line), "TRIPPED %s at %s %s",
             m_tripHigh ? "HIGH" : "LOW", vb, m_spec.units);
    labelCol = p.bad;
  } else {
    fmt(valOf(m_curY), vb, sizeof(vb));
    snprintf(line, sizeof(line), "%s  CUR %s", m_spec.metricLabel, vb);
  }
  ui::tft.setTextFont(1);
  ui::tft.setTextDatum(TL_DATUM);
  ui::tft.setTextColor(labelCol, p.card);
  ui::tft.drawString(line, CX + 8, CY + 5);

  // State chip.
  const char *chip;
  uint16_t chipCol;
  if (m_latched) {
    chip = "TRIP";
    chipCol = p.bad;
  } else if (!hiArmed() && !loArmed()) {
    chip = "OFF";
    chipCol = p.muted;
  } else {
    chip = "ARMED";
    chipCol = p.ok;
  }
  const int chipX = CX + 190, chipY = CY + 3, chipW = 54, chipH = 14;
  ui::tft.fillRoundRect(chipX, chipY, chipW, chipH, 4, p.card_hi);
  ui::tft.drawRoundRect(chipX, chipY, chipW, chipH, 4, chipCol);
  ui::tft.setTextFont(1);
  ui::tft.setTextDatum(MC_DATUM);
  ui::tft.setTextColor(chipCol, p.card_hi);
  ui::tft.drawString(chip, chipX + chipW / 2, chipY + chipH / 2);
  ui::tft.setTextDatum(TL_DATUM);

  // Band read-out. A parked edge reads "off" in words as well as position.
  ui::tft.setTextFont(1);
  if (hiArmed()) {
    fmt(valOf(m_hiY), vb, sizeof(vb));
  } else {
    snprintf(vb, sizeof(vb), "off");
  }
  snprintf(line, sizeof(line), "HI %s", vb);
  ui::tft.setTextColor(m_grab == Grab::High ? p.bad
                                            : (hiArmed() ? p.warn : p.muted),
                       p.card);
  ui::tft.drawString(line, CX + 252, CY + 4);

  if (loArmed()) {
    fmt(valOf(m_loY), vb, sizeof(vb));
  } else {
    snprintf(vb, sizeof(vb), "off");
  }
  snprintf(line, sizeof(line), "LO %s", vb);
  ui::tft.setTextColor(m_grab == Grab::Low ? p.bad
                                           : (loArmed() ? p.warn : p.muted),
                       p.card);
  ui::tft.drawString(line, CX + 252, CY + 16);

  drawValue();
}

void LoggerScreen::drawFooter() {
  const auto &p = theme::c();
  const int y = ui::tft.height() - ui::FOOTER_H;
  ui::tft.fillRect(0, y, ui::tft.width(), ui::FOOTER_H, p.bg);

  char hint[64];
  snprintf(hint, sizeof(hint), "C:back A:%s B:dismiss PRESS:%s",
           logger::isOpen() ? "stop" : "log",
           m_grab == Grab::None ? "grab" : "release");
  ui::tft.setTextFont(1);
  ui::tft.setTextDatum(TL_DATUM);
  ui::tft.setTextColor(p.muted, p.bg);
  ui::tft.drawString(hint, 6, y + 3);

  if (logger::isOpen()) {
    const char *path = logger::path();
    const char *base = strrchr(path, '/');
    ui::tft.setTextDatum(TR_DATUM);
    ui::tft.setTextColor(p.ok, p.bg);
    ui::tft.drawString(base ? base + 1 : path, ui::tft.width() - 6, y + 3);
    ui::tft.setTextDatum(TL_DATUM);
  }
}

// --- lifecycle -------------------------------------------------------------

void LoggerScreen::enter() {
  m_spec = spec();
  buzzer::begin();

  ui::header(m_spec.title, nullptr); // footer is ours, drawn below
  ui::clearBody();

  m_ready = openSensor();
  if (!m_ready) {
    ui::note(0, unavailableText(), theme::c().warn);
    drawFooter();
    return;
  }

  // Thresholds are session-scoped, NOT screen-scoped: they survive a walk back
  // to the launcher and die at power-off. Both rails on first entry = disabled.
  if (!m_bandInit) {
    m_hiY = 0;
    m_loY = GH - 1;
    m_curY = GH / 2;
    m_bandInit = true;
  }

  for (int i = 0; i < GW; ++i) m_col[i] = EMPTY;
  m_x = 0;
  m_have = false;
  m_next = 0;
  m_nextValueMs = 0;
  m_grab = Grab::None;
  m_step = 1;

  drawFrame();
  redrawGraph();
  drawCard();
  drawFooter();
}

void LoggerScreen::exit() {
  buzzer::stop();
  if (logger::isOpen()) logger::close();
  closeSensor();
  // m_latched is deliberately NOT cleared. A latch you walked away from is
  // still a latch when you come back -- that is the whole point of latching.
}

void LoggerScreen::tick(uint32_t nowMs) {
  buzzer::tick(nowMs);
  if (!m_ready) return;

  if (m_latched) {
    if ((int32_t)(nowMs - m_nextFlashMs) >= 0) {
      m_nextFlashMs = nowMs + FLASH_MS;
      m_flashOn = !m_flashOn;
      drawFrame();
    }
    if ((int32_t)(nowMs - m_nextChirpMs) >= 0) {
      m_nextChirpMs = nowMs + CHIRP_PERIOD_MS;
      buzzer::pattern(CHIRP_SEQ, 1);
    }
  }

  if ((int32_t)(nowMs - m_next) < 0) return;
  m_next = nowMs + m_spec.intervalMs;

  float v = 0.0f;
  if (!sample(v)) return;
  m_last = v;
  m_have = true;

  if (logger::isOpen()) {
    char extra[48], line[96];
    logExtra(extra, sizeof(extra));
    snprintf(line, sizeof(line), "%lu,%.*f%s", (unsigned long)nowMs,
             (int)m_spec.decimals, (double)v, extra);
    logger::writeLine(line);
  }

  evaluate(nowMs, v);

  // Sweep-scroll: write one column, blank a short gap ahead of the write head
  // so new data is visually separated from the previous pass.
  m_col[m_x] = (uint8_t)yOf(v);
  drawColumn(m_x);
  m_x = (m_x + 1) % GW;
  for (int i = 0; i < 3; ++i) {
    const int gx = (m_x + i) % GW;
    m_col[gx] = EMPTY;
    drawColumn(gx);
  }

  if ((int32_t)(nowMs - m_nextValueMs) >= 0) {
    m_nextValueMs = nowMs + VALUE_MS;
    drawValue();
  }
}

// --- interaction -----------------------------------------------------------

void LoggerScreen::onButton(Btn b) {
  if (onExtraButton(b)) return;
  const uint32_t now = millis();
  switch (b) {
  case Btn::C: ui::pop(); break;
  case Btn::A: toggleLog(); break;
  case Btn::B: dismiss(); break;
  case Btn::Up: if (m_ready) moveCursor(-1, now); break;
  case Btn::Down: if (m_ready) moveCursor(+1, now); break;
  case Btn::Press: if (m_ready) toggleGrab(); break;
  default: break;
  }
}

void LoggerScreen::moveCursor(int dir, uint32_t nowMs) {
  // Two speeds compose here: input::poll() repeats a HELD direction at an
  // accelerating rate, and a run of closely-spaced events grows the step. A
  // deliberate tap is always exactly one pixel; a held switch sweeps the pane
  // in about a second. The step cap stays low precisely because the repeat
  // rate already supplies the speed -- both ramping at once overshoots.
  if (nowMs - m_lastMoveMs < 180) {
    if (m_step < 4) m_step <<= 1;
  } else {
    m_step = 1;
  }
  m_lastMoveMs = nowMs;

  int y = m_curY + dir * m_step;
  if (y < 0) y = 0;
  if (y > GH - 1) y = GH - 1;

  // A grabbed marker follows the cursor, and may not cross its partner. Note
  // that neither can be pushed onto the OTHER's rail, so "at a rail" stays an
  // unambiguous reading of "this edge is disabled".
  if (m_grab == Grab::High) {
    if (y > m_loY - MIN_GAP) y = m_loY - MIN_GAP;
    if (y < 0) y = 0;
    m_hiY = y;
  } else if (m_grab == Grab::Low) {
    if (y < m_hiY + MIN_GAP) y = m_hiY + MIN_GAP;
    if (y > GH - 1) y = GH - 1;
    m_loY = y;
  }
  m_curY = y;

  redrawGraph();
  drawCard();
}

void LoggerScreen::toggleGrab() {
  if (m_grab != Grab::None) {
    m_grab = Grab::None; // release where it sits -- a rail commits "disabled"
  } else {
    const int dh = abs(m_curY - m_hiY);
    const int dl = abs(m_curY - m_loY);
    if (dh > GRAB_TOL && dl > GRAB_TOL) return; // nothing in reach
    m_grab = (dh <= dl) ? Grab::High : Grab::Low; // ties go to the closer line
    m_curY = (m_grab == Grab::High) ? m_hiY : m_loY;
  }
  redrawGraph();
  drawCard();
  drawFooter();
}

// --- band evaluation, latch, SD --------------------------------------------

void LoggerScreen::logEvent(const char *what, uint32_t nowMs, float v) {
  // A breach only reaches the card if a capture is running. The ALERT always
  // fires; the paper trail is the user's call, made with A.
  if (!logger::isOpen()) return;
  char line[112];
  snprintf(line, sizeof(line), "# %s t=%lu value=%.*f hi=%s lo=%s", what,
           (unsigned long)nowMs, (int)m_spec.decimals, (double)v,
           hiArmed() ? "armed" : "off", loArmed() ? "armed" : "off");
  logger::writeLine(line);
}

void LoggerScreen::evaluate(uint32_t nowMs, float v) {
  const bool overHi = hiArmed() && v > valOf(m_hiY);
  const bool underLo = loArmed() && v < valOf(m_loY);
  const bool breach = overHi || underLo; // band model: breach = band EXIT (§3)

  if (breach && !m_latched) {
    m_latched = true;
    m_tripHigh = overHi;
    m_tripValue = v;
    m_inBand = false;
    m_flashOn = true;
    m_nextFlashMs = nowMs + FLASH_MS;
    m_nextChirpMs = nowMs + CHIRP_PERIOD_MS;
    buzzer::pattern(TRIP_SEQ, sizeof(TRIP_SEQ) / sizeof(TRIP_SEQ[0]));
    logEvent(overHi ? "BREACH-HIGH" : "BREACH-LOW", nowMs, v);
    Serial.print("[alert] ");
    Serial.print(m_spec.title);
    Serial.print(overHi ? " HIGH " : " LOW ");
    Serial.println(v, 4);
    drawFrame();
    drawCard();
  } else if (breach) {
    m_inBand = false;
  } else if (!m_inBand) {
    // Back inside the band while still latched. Recorded, because "it came
    // back" is exactly the fact a latch exists to stop you assuming.
    m_inBand = true;
    logEvent("INBAND", nowMs, v);
  }
}

void LoggerScreen::dismiss() {
  if (!m_latched) return;
  m_latched = false;
  m_flashOn = false;
  buzzer::stop();
  logEvent("DISMISS", millis(), m_last);
  drawFrame();
  drawCard();
}

void LoggerScreen::toggleLog() {
  if (logger::isOpen()) {
    logger::close();
  } else if (!logger::open(m_spec.logPrefix)) {
    // Reported in the footer, NOT via ui::note -- row 0 lands inside the graph
    // pane, where the message would sit on top of the trace until the sweep
    // happened to wipe it.
    const auto &p = theme::c();
    const int y = ui::tft.height() - ui::FOOTER_H;
    ui::tft.fillRect(0, y, ui::tft.width(), ui::FOOTER_H, p.bg);
    ui::tft.setTextFont(1);
    ui::tft.setTextDatum(TL_DATUM);
    ui::tft.setTextColor(p.bad, p.bg);
    ui::tft.drawString("log open failed -- no SD card, or no space", 6, y + 3);
    return;
  } else {
    logger::writeLine(m_spec.logHeader);
    char hdr[112];
    snprintf(hdr, sizeof(hdr), "# axis=%.2f..%.2f %s  clock=%s",
             (double)m_spec.axisMin, (double)m_spec.axisMax, m_spec.units,
             wallclock::qualityName());
    logger::writeLine(hdr);
  }
  drawFooter();
}
