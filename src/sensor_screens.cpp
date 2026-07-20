#include "logger_tile.h"

#include <math.h>

#include "LIS3DHTR.h"

// ===========================================================================
// The three loggers  (Light-SensorLoggers-TODO.md)
// ===========================================================================
//
// Every screen below is a LoggerScreen: it supplies a sample() and an axis and
// inherits the graph, the grab-cursor band editor, the latch and the SD sink.
// If one of these files starts growing UI code, the framework has been bypassed
// and that is the bug -- fix logger_tile.cpp instead.

namespace {

// --- shared analog front end ----------------------------------------------

constexpr int ADC_FULL = 4095; // 12-bit; set per-screen in openSensor()

void beginAnalog() { analogReadResolution(12); }

// Mic level as dBFS -- relative to full scale, which is an honest thing to
// call a number a $0.30 electret produced. Peak-to-peak over a short burst
// rejects the capsule's DC bias without needing to know what it is.
constexpr int MIC_BURST = 200;

float micDbfs() {
  int lo = ADC_FULL, hi = 0;
  for (int i = 0; i < MIC_BURST; ++i) {
    const int v = analogRead(WIO_MIC);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  float amp = (float)(hi - lo) / (float)ADC_FULL;
  if (amp < 1.0e-4f) amp = 1.0e-4f; // floor the log at -80 dBFS
  return 20.0f * log10f(amp);
}

// Sound axis, in dBFS before any calibration offset is applied.
constexpr float SND_FLOOR = -60.0f;
constexpr float SND_CEIL = 0.0f;

// ===========================================================================
// Vibration  (LIS3DHTR)
// ===========================================================================

class VibrationScreen : public LoggerScreen {
protected:
  LoggerSpec spec() const override {
    return {"Vibration", "RMS DEV", "g", "vib",
            "# t_ms,rms_g,x_g,y_g,z_g", 20, 0.0f, 0.5f, 4};
  }

  const char *unavailableText() const override {
    return "no accelerometer detected";
  }

  bool openSensor() override {
    m_dev = nullptr;
    for (uint8_t i = 0; i < inventory::count; ++i) {
      if (inventory::devs[i].cap == i2cbus::Cap::Accel) {
        m_dev = &inventory::devs[i];
        break;
      }
    }
    if (!m_dev) return false;
    m_lis.begin(*m_dev->bus, m_dev->addr);
    m_lis.setOutputDataRate(LIS3DHTR_DATARATE_100HZ);
    m_lis.setFullScaleRange(LIS3DHTR_RANGE_4G);
    m_sumSq = 0.0f;
    return true;
  }

  bool sample(float &out) override {
    m_lis.getAcceleration(&m_x, &m_y, &m_z);
    const float mag = sqrtf(m_x * m_x + m_y * m_y + m_z * m_z);
    const float dev = fabsf(mag - 1.0f); // deviation from rest, in g

    // Exponentially weighted mean square: a running RMS with no window buffer.
    m_sumSq = m_sumSq * 0.95f + dev * dev * 0.05f;
    out = sqrtf(m_sumSq);
    return true;
  }

  void logExtra(char *out, size_t n) override {
    snprintf(out, n, ",%.4f,%.4f,%.4f", (double)m_x, (double)m_y, (double)m_z);
  }

private:
  Detected *m_dev = nullptr;
  LIS3DHTR<TwoWire> m_lis;
  float m_sumSq = 0.0f;
  float m_x = 0.0f, m_y = 0.0f, m_z = 0.0f;
};

// ===========================================================================
// Sound level  (onboard mic, §4 calibration)
// ===========================================================================

class SoundScreen : public LoggerScreen {
protected:
  LoggerSpec spec() const override {
    // A valid CAL record shifts the whole axis by one offset and relabels the
    // units. No record, no dB SPL -- the axis stays honestly relative (§4).
    if (soundcal::valid()) {
      const float off = soundcal::get().offset;
      return {"Sound", "LEVEL cal(1pt)", "~dB SPL", "snd",
              "# t_ms,spl_approx_db,dbfs_raw", 50,
              SND_FLOOR + off, SND_CEIL + off, 1};
    }
    return {"Sound", "LEVEL rel", "dBFS", "snd",
            "# t_ms,dbfs,dbfs_raw", 50, SND_FLOOR, SND_CEIL, 1};
  }

  bool openSensor() override {
    beginAnalog();
    return true;
  }

  bool sample(float &out) override {
    m_raw = micDbfs();
    out = soundcal::valid() ? m_raw + soundcal::get().offset : m_raw;
    return true;
  }

  // The uncalibrated dBFS goes into the record alongside the displayed value,
  // so a capture taken today stays re-derivable if the cal is redone tomorrow.
  void logExtra(char *out, size_t n) override {
    snprintf(out, n, ",%.1f", (double)m_raw);
  }

  bool onExtraButton(Btn b) override;

private:
  float m_raw = 0.0f;
};

// ---------------------------------------------------------------------------
// Sound reference calibration  (§4 capture flow)
// ---------------------------------------------------------------------------

class SoundCalScreen : public Screen {
public:
  const char *title() const override { return "Sound CAL"; }

  void enter() override {
    beginAnalog();
    m_capturing = false;
    m_status[0] = '\0';
    m_next = 0;
    if (!soundcal::valid()) {
      m_refDb = 94.0f; // the calibrator tone almost everyone actually owns
    } else {
      m_refDb = soundcal::get().refDb;
    }
    render();
  }

  void tick(uint32_t now) override {
    if ((int32_t)(now - m_next) < 0) return;
    m_next = now + 100;

    const float dbfs = micDbfs();
    const auto &p = theme::c();
    char buf[56];

    if (m_capturing) {
      m_sum += dbfs;
      m_n++;
      if ((int32_t)(now - m_endMs) >= 0) {
        finish();
        return;
      }
      snprintf(buf, sizeof(buf), "capturing... %lu samples",
               (unsigned long)m_n);
      ui::note(3, buf, p.warn);
      return;
    }

    snprintf(buf, sizeof(buf), "live     %6.1f dBFS", (double)dbfs);
    ui::note(3, buf, p.fg);
  }

  void onButton(Btn b) override {
    if (m_capturing) return; // a capture window runs to completion
    switch (b) {
    case Btn::C: ui::pop(); break;
    case Btn::Up:
      if (m_refDb < 140.0f) m_refDb += 1.0f;
      renderRef();
      break;
    case Btn::Down:
      if (m_refDb > 30.0f) m_refDb -= 1.0f;
      renderRef();
      break;
    case Btn::Press: start(); break;
    default: break;
    }
  }

private:
  bool m_capturing = false;
  float m_refDb = 94.0f;
  float m_sum = 0.0f;
  uint32_t m_n = 0;
  uint32_t m_endMs = 0;
  uint32_t m_next = 0;
  char m_status[64];

  static constexpr uint32_t WINDOW_MS = 2000;

  void render() {
    const auto &p = theme::c();
    ui::header("Sound CAL", "C:back  UP/DOWN:ref dB  PRESS:capture");
    ui::clearBody();
    ui::note(0, "Play a known reference source and hold it", p.fg);
    ui::note(1, "steady, dial its level in, then PRESS.", p.fg);

    // The caveat is baked into the flow, not buried in a manual (§4).
    ui::note(6, "Single-point offset only: this assumes the", p.muted);
    ui::note(7, "mic is linear in the log domain. It is an", p.muted);
    ui::note(8, "approximation, not a lab meter -- values", p.muted);
    ui::note(9, "read \"~dB SPL\" and never plain dB SPL.", p.muted);

    if (soundcal::valid()) {
      char buf[56];
      snprintf(buf, sizeof(buf), "current offset %+.1f dB (ref %.0f dB)",
               (double)soundcal::get().offset, (double)soundcal::get().refDb);
      ui::note(11, buf, p.ok);
    } else {
      ui::note(11, "no calibration record -- level is relative", p.muted);
    }
    renderRef();
  }

  void renderRef() {
    char buf[56];
    snprintf(buf, sizeof(buf), "ref      %6.0f dB SPL", (double)m_refDb);
    ui::note(4, buf, theme::c().accent);
  }

  void start() {
    m_capturing = true;
    m_sum = 0.0f;
    m_n = 0;
    m_endMs = millis() + WINDOW_MS;
    ui::note(10, "", theme::c().bg);
  }

  void finish() {
    m_capturing = false;
    const auto &p = theme::c();
    if (m_n == 0) {
      ui::note(10, "capture produced no samples", p.bad);
      return;
    }
    const float avg = m_sum / (float)m_n;
    const bool ok = soundcal::save(m_refDb, avg);
    snprintf(m_status, sizeof(m_status), "%s  offset %+.1f dB",
             ok ? "saved /cal/sound.json" : "SAVE FAILED (no SD?)",
             (double)(m_refDb - avg));
    render(); // repaint so the "current offset" line reflects the new record
    ui::note(10, m_status, ok ? p.ok : p.bad);
  }
};

SoundCalScreen s_soundCal;

bool SoundScreen::onExtraButton(Btn b) {
  // LEFT is the only free key on a logger screen; calibration is a Sound-only
  // affordance so it lives here rather than in the shared framework.
  if (b != Btn::Left) return false;
  ui::push(&s_soundCal);
  return true;
}

// ===========================================================================
// Ambient light  (onboard light sensor)
// ===========================================================================

class LightScreen : public LoggerScreen {
protected:
  LoggerSpec spec() const override {
    // Relative, and labelled relative. The onboard sensor has no lux transfer
    // function published for it, so calling this "lux" would be an invention.
    return {"Ambient Light", "LIGHT rel", "%", "lux",
            "# t_ms,rel_pct,raw_counts", 250, 0.0f, 100.0f, 1};
  }

  bool openSensor() override {
    beginAnalog();
    return true;
  }

  bool sample(float &out) override {
    m_raw = analogRead(WIO_LIGHT);
    out = 100.0f * (float)m_raw / (float)ADC_FULL;
    return true;
  }

  void logExtra(char *out, size_t n) override {
    snprintf(out, n, ",%d", m_raw);
  }

private:
  int m_raw = 0;
};

VibrationScreen s_vibration;
SoundScreen s_sound;
LightScreen s_light;

} // namespace

Screen *vibrationScreen() { return &s_vibration; }
Screen *soundScreen() { return &s_sound; }
Screen *lightScreen() { return &s_light; }
