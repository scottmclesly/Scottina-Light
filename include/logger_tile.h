#ifndef SL_LOGGER_TILE_H
#define SL_LOGGER_TILE_H

#include "scottina_light.h"

// ---------------------------------------------------------------------------
// Shared logger framework  (Light-SensorLoggers-TODO.md §0)
// ---------------------------------------------------------------------------
//
// One pathway, three consumers: vibration, sound level, ambient light. A
// subclass supplies a sample() and a LoggerSpec describing its axis; everything
// else -- graph-dominant layout, the grab-cursor band editor, park-to-disable,
// latching buzzer/flash alerts and the SD sink -- is built once, here.
//
// Deliberate asymmetry, stated where it is implemented rather than only in the
// to-do: THRESHOLDS ARE SESSION-SCOPED. They live in RAM and reset to their
// rails on boot, because setting one takes four seconds with a thumb switch.
// Sound CALIBRATION persists to SD, because redoing it needs a reference source
// you may not have with you. See soundcal below.

// ---------------------------------------------------------------------------
// Buzzer  (§3 -- the cue Prime does not have)
// ---------------------------------------------------------------------------

namespace buzzer {
void begin();
// Non-blocking. `seq` alternates ON,OFF,ON,... durations in ms, starting ON.
// The array must outlive the pattern -- pass a static.
void pattern(const uint16_t *seq, uint8_t n);
void stop();
void tick(uint32_t nowMs);
} // namespace buzzer

// ---------------------------------------------------------------------------
// Sound reference calibration  (§4 -- persists to /cal/sound.json)
// ---------------------------------------------------------------------------
//
// Single-point offset in the log domain: dB_SPL ~= dBFS + offset. That is an
// APPROXIMATION and the UI says so; it assumes the mic is linear in the log
// domain, which a $0.30 electret is only roughly. Never presented as a meter.

namespace soundcal {

struct Record {
  float refDb;      // the reference level the user vouched for
  float refDbfs;    // what Light measured while that reference played
  float offset;     // refDb - refDbfs
  uint64_t captured;
  bool valid;
};

const Record &get();
bool valid();
// Boot load. A malformed or missing record leaves valid() false and the sound
// logger honestly relative -- never a crash, never a faked dB.
bool load();
bool save(float refDb, float refDbfs);

} // namespace soundcal

// ---------------------------------------------------------------------------
// LoggerScreen
// ---------------------------------------------------------------------------

struct LoggerSpec {
  const char *title;
  const char *metricLabel; // muted label on the compact card
  const char *units;
  const char *logPrefix;   // /logs/<prefix>NNN.log
  const char *logHeader;   // CSV comment written at capture start
  uint32_t intervalMs;     // per-logger tick rate (§0)
  float axisMin, axisMax;  // graph full scale, in display units
  uint8_t decimals;
};

class LoggerScreen : public Screen {
public:
  const char *title() const override { return spec().title; }

  void enter() override;
  void exit() override;
  void tick(uint32_t nowMs) override;
  void onButton(Btn b) override;

protected:
  // --- subclass contract ---------------------------------------------------

  virtual LoggerSpec spec() const = 0;

  // Bind/release the sensor. openSensor() returns false when the hardware is
  // not there; the screen then says so instead of drawing a lying graph.
  virtual bool openSensor() { return true; }
  virtual void closeSensor() {}
  virtual const char *unavailableText() const { return "sensor not detected"; }

  // One reading in display units. false = nothing new this tick.
  virtual bool sample(float &out) = 0;

  // Extra CSV columns after the value; default writes none.
  virtual void logExtra(char *out, size_t n) { if (n) out[0] = '\0'; }

  // Called before the base handles a button, so a subclass can claim a key
  // (Sound claims LEFT for its calibration screen). Return true to swallow it.
  virtual bool onExtraButton(Btn b) { (void)b; return false; }

  float lastValue() const { return m_last; }
  bool haveSample() const { return m_have; }

private:
  // Graph pane: full width, most of the height (§1 -- graph gets the estate).
  static constexpr int GX = 6;
  static constexpr int GY = 38;
  static constexpr int GW = 308;
  static constexpr int GH = 138;
  static constexpr uint8_t EMPTY = 0xFF;

  // Compact metric card beneath it.
  static constexpr int CX = 6;
  static constexpr int CY = 180;
  static constexpr int CW = 308;
  static constexpr int CH = 46;

  static constexpr int GRAB_TOL = 10; // px; easy to catch, hard to mis-catch
  static constexpr int MIN_GAP = 4;   // px the markers may not close past

  enum class Grab : uint8_t { None = 0, High, Low };

  LoggerSpec m_spec{};
  bool m_ready = false;

  // Sweep-scroll trace: one byte per column, EMPTY where nothing has been
  // written yet. Sweeping beats memmove-scrolling by 307 columns of SPI per
  // sample -- the dirty-rect discipline the to-do asks for (§0).
  uint8_t m_col[GW];
  int m_x = 0;
  uint32_t m_next = 0;
  uint32_t m_nextValueMs = 0;
  float m_last = 0.0f;
  bool m_have = false;

  // Band state. Rails ARE the disabled state -- there is no separate toggle,
  // so the marker's position always tells the whole truth (§1 park-to-disable).
  bool m_bandInit = false;
  int m_hiY = 0;           // 0 == top rail == HIGH disabled
  int m_loY = GH - 1;      // GH-1 == bottom rail == LOW disabled
  int m_curY = GH / 2;
  Grab m_grab = Grab::None;

  // Cursor acceleration: input::poll() is edge-triggered with no auto-repeat,
  // so 138 taps would be needed to cross the pane. Held/spun presses grow the
  // step; a deliberate tap stays 1 px.
  uint32_t m_lastMoveMs = 0;
  int m_step = 1;

  bool m_latched = false;
  bool m_tripHigh = false;
  float m_tripValue = 0.0f;
  uint32_t m_nextChirpMs = 0;
  uint32_t m_nextFlashMs = 0;
  bool m_flashOn = false;
  bool m_inBand = true;

  bool hiArmed() const { return m_hiY > 0; }
  bool loArmed() const { return m_loY < GH - 1; }

  int yOf(float v) const;
  float valOf(int y) const;
  void fmt(float v, char *out, size_t n) const;

  void drawFrame();
  void drawTraceColumn(int x);
  void drawColumn(int x);   // trace + per-pixel overlay, for the sweep head
  void overlayColumn(int x);
  void drawMarkerLine(int y, uint16_t col, int phase);
  void redrawGraph();
  uint16_t hiColour() const;
  uint16_t loColour() const;
  void drawCard();
  void drawValue();
  void drawFooter();

  void moveCursor(int delta, uint32_t nowMs);
  void toggleGrab();
  void evaluate(uint32_t nowMs, float v);
  void logEvent(const char *what, uint32_t nowMs, float v);
  void dismiss();
  void toggleLog();
};

#endif // SL_LOGGER_TILE_H
