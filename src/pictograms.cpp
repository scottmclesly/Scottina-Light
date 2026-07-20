#include "pictograms.h"

#include <math.h>

#include "scottina_light.h"
#include "theme.h"

namespace pict {

namespace {

TFT_eSPI &g() { return ui::tft; }

float lwOf(int r) { return fmaxf(2.0f, roundf(r / 5.0f)); }

void wline(float x0, float y0, float x1, float y1, float w, uint16_t c, uint16_t bg) {
  g().drawWideLine(x0, y0, x1, y1, w, c, bg);
}

void dot(float x, float y, float rr, uint16_t c, uint16_t bg) {
  g().fillSmoothCircle((int32_t)lroundf(x), (int32_t)lroundf(y),
                       (int32_t)fmaxf(1.0f, roundf(rr)), c, bg);
}

// A ring of thickness lw. drawArc's inner radius gives the stroke width.
void ring(float cx, float cy, float rr, float lw, uint16_t c, uint16_t bg) {
  const int32_t ro = (int32_t)lroundf(rr);
  const int32_t ri = (int32_t)fmaxf(1.0f, roundf(rr - lw));
  g().drawArc((int32_t)lroundf(cx), (int32_t)lroundf(cy), ro, ri, 0, 360, c, bg, true);
}

void box(float x0, float y0, float x1, float y1, uint16_t c) {
  g().fillRect((int32_t)lroundf(x0), (int32_t)lroundf(y0),
               (int32_t)lroundf(x1 - x0), (int32_t)lroundf(y1 - y0), c);
}

// Point on the circle at PIL-style angle (0 deg = 3 o'clock, clockwise).
void pt(float cx, float cy, float r, float deg, float &x, float &y) {
  const float a = deg * (float)M_PI / 180.0f;
  x = cx + r * cosf(a);
  y = cy + r * sinf(a);
}

// ------------------------------------------------------------------ glyphs --

// Address matrix with one responding device.
void gI2c(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float step = r * 0.62f;
  for (int gr = -1; gr <= 1; ++gr) {
    for (int gc = -1; gc <= 1; ++gc) {
      const float x = cx + gc * step, y = cy + gr * step;
      if (gr == 0 && gc == 1) {
        const float s = r * 0.30f;
        box(x - s, y - s, x + s, y + s, c);
      } else {
        dot(x, y, r * 0.12f, c, bg);
      }
    }
  }
}

// Differential bus pair with tapped nodes.
void gCan(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float lw = lwOf((int)r);
  const float y0 = cy - r * 0.30f, y1 = cy + r * 0.30f;
  wline(cx - r, y0, cx + r, y0, lw, c, bg);
  wline(cx - r, y1, cx + r, y1, lw, c, bg);
  const float fx[3] = {-0.55f, 0.0f, 0.55f};
  const bool up[3] = {true, false, true};
  for (int i = 0; i < 3; ++i) {
    const float x = cx + r * fx[i];
    const float y = up[i] ? y0 - r * 0.45f : y1 + r * 0.45f;
    wline(x, up[i] ? y0 : y1, x, y, lw, c, bg);
    dot(x, y, r * 0.16f, c, bg);
  }
}

// Opposed TX/RX triangles across the gate (airlock homage).
void gSerial(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float lw = lwOf((int)r);
  wline(cx, cy - r * 0.9f, cx, cy + r * 0.9f, lw, c, bg);
  const float h = r * 0.52f;
  g().fillTriangle((int32_t)(cx - r), (int32_t)(cy - h), (int32_t)(cx - r),
                   (int32_t)(cy + h), (int32_t)(cx - r * 0.25f), (int32_t)cy, c);
  g().fillTriangle((int32_t)(cx + r), (int32_t)(cy - h), (int32_t)(cx + r),
                   (int32_t)(cy + h), (int32_t)(cx + r * 0.25f), (int32_t)cy, c);
}

// Damped oscillation leaving a struck mass -- machinery ringing down.
void gVibration(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float lw = lwOf((int)r);
  constexpr int N = 17;
  float px = 0, py = 0;
  for (int i = 0; i < N; ++i) {
    const float t = (float)i / (N - 1);
    const float x = cx - r + 2.0f * r * t;
    const float y = cy - (r * 0.75f) * sinf(2.0f * (float)M_PI * 2.5f * t) *
                             expf(-1.8f * t);
    if (i) wline(px, py, x, y, lw, c, bg);
    px = x;
    py = y;
  }
  const float s = r * 0.22f;
  box(cx - r - s * 0.5f, cy - s, cx - r + s * 1.5f, cy + s, c);
}

// Acoustic emission: a struck diaphragm throwing three wavefronts.
//
// Same construction as gVibration -- a solid mass on the left, energy leaving
// it rightward -- so the two tiles read as siblings. Vibration carries the
// energy as a damped wave through the body; Sound radiates it as free air.
void gSound(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float lw = lwOf((int)r);
  const float s = r * 0.30f;
  box(cx - r - s * 0.4f, cy - s * 1.7f, cx - r + s * 1.6f, cy + s * 1.7f, c);

  // Three arcs, opening right, spaced like the wave crests next door.
  for (int i = 0; i < 3; ++i) {
    const float rr = r * (0.42f + 0.29f * (float)i);
    const int32_t ro = (int32_t)lroundf(rr);
    const int32_t ri = (int32_t)fmaxf(1.0f, roundf(rr - lw));
    // drawArc measures from 12 o'clock clockwise: 45..135 is the right quadrant.
    g().drawArc((int32_t)lroundf(cx - r * 0.55f), (int32_t)lroundf(cy), ro, ri,
                45, 135, c, bg, true);
  }
}

// Luminance: a solid source with radiating output.
//
// Deliberately NOT gSettings' ring-with-ticks -- that one is hollow with the
// ticks welded on. This is a filled disc whose rays stand clear of it, so the
// pair never collide at tile size.
void gLight(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float lw = lwOf((int)r);
  dot(cx, cy, r * 0.40f, c, bg);
  for (int a = 0; a < 360; a += 45) {
    float ax, ay, bx, by;
    pt(cx, cy, r * 0.62f, (float)a, ax, ay); // gap between disc and ray
    pt(cx, cy, r * 0.98f, (float)a, bx, by);
    wline(ax, ay, bx, by, lw, c, bg);
  }
}

// Capture stack: a framed window with recorded bars.
void gLogs(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float lw = lwOf((int)r);
  const float x0 = cx - r * 0.75f, x1 = cx + r * 0.75f;
  const float y0 = cy - r * 0.90f, y1 = cy + r * 0.90f;
  wline(x0, y0, x1, y0, lw, c, bg);
  wline(x0, y1, x1, y1, lw, c, bg);
  wline(x0, y0, x0, y1, lw, c, bg);
  wline(x1, y0, x1, y1, lw, c, bg);
  const float inner = (x1 - x0) - lw * 2.0f;
  const float bh = fmaxf(2.0f, r * 0.13f);
  const float frac[3] = {0.88f, 0.50f, 0.72f};
  for (int i = 0; i < 3; ++i) {
    const float by = cy + (i - 1) * r * 0.38f;
    box(x0 + lw, by - bh * 0.5f, x0 + lw + inner * frac[i], by + bh * 0.5f, c);
  }
}

// Maintenance: ring with radial adjustment ticks.
void gSettings(float cx, float cy, float r, uint16_t c, uint16_t bg) {
  const float lw = lwOf((int)r);
  ring(cx, cy, r * 0.62f, lw, c, bg);
  for (int a = 0; a < 360; a += 45) {
    float ax, ay, bx, by;
    pt(cx, cy, r * 0.62f, (float)a, ax, ay);
    pt(cx, cy, r, (float)a, bx, by);
    wline(ax, ay, bx, by, lw, c, bg);
  }
}

// The multi-tool of the mother's splash: blades fanned behind a red body,
// emblem ring on the face.
void gKnife(float cx, float cy, float r, uint16_t /*c*/, uint16_t bg) {
  const auto &p = theme::c();
  const float lw = fmaxf(2.0f, r / 7.0f);
  const float L = r * 1.35f;

  const float ang[3] = {-38.0f, 0.0f, 38.0f};
  for (int i = 0; i < 3; ++i) {
    const uint16_t col = (i == 1) ? p.fg : p.muted;
    float ex, ey;
    pt(cx + r * 0.22f, cy, L, ang[i], ex, ey);
    wline(cx + r * 0.22f, cy, ex, ey, lw, col, bg);
    pt(cx - r * 0.22f, cy, L, 180.0f - ang[i], ex, ey);
    wline(cx - r * 0.22f, cy, ex, ey, lw, col, bg);
  }

  const float bw = r * 0.28f, bh = r * 0.82f;
  g().fillRoundRect((int32_t)(cx - bw), (int32_t)(cy - bh), (int32_t)(bw * 2),
                    (int32_t)(bh * 2), (int32_t)fmaxf(3.0f, r * 0.12f), p.bad);

  ring(cx, cy, r * 0.34f, lw * 0.8f, p.accent, p.bad);
  const float t = r * 0.06f, arm = r * 0.17f;
  box(cx - t, cy - arm, cx + t, cy + arm, p.accent);
  box(cx - arm, cy - t, cx + arm, cy + t, p.accent);
}

} // namespace

void draw(Glyph glyph, int cx, int cy, int r, uint16_t color, uint16_t bg) {
  const float fx = (float)cx, fy = (float)cy, fr = (float)r;
  switch (glyph) {
  case Glyph::I2c: gI2c(fx, fy, fr, color, bg); break;
  case Glyph::Can: gCan(fx, fy, fr, color, bg); break;
  case Glyph::Serial: gSerial(fx, fy, fr, color, bg); break;
  case Glyph::Vibration: gVibration(fx, fy, fr, color, bg); break;
  case Glyph::Sound: gSound(fx, fy, fr, color, bg); break;
  case Glyph::Light: gLight(fx, fy, fr, color, bg); break;
  case Glyph::Logs: gLogs(fx, fy, fr, color, bg); break;
  case Glyph::Settings: gSettings(fx, fy, fr, color, bg); break;
  case Glyph::Knife: gKnife(fx, fy, fr, color, bg); break;
  default:
    // Unknown keys degrade to the mother's original filled dot.
    dot(fx, fy, fr * 0.5f, color, bg);
    break;
  }
}

} // namespace pict
