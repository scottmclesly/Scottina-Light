#include "scottina_light.h"

// Boot splash, after ScottinaSplash.png in the mother project: green phosphor
// rain, the wordmark, the multi-tool, and a progress bar under "INITIATING
// SYSTEM...".
//
// The bar is not decorative -- each segment is a real boot step (bus init, card
// mount, scan). Rain is confined to two side bands so the centre column never
// needs repainting; nothing flickers behind the text.

namespace splash {

namespace {

constexpr int W = 320;
constexpr int H = 240;

constexpr int CELL_W = 8;
constexpr int CELL_H = 10;
constexpr int BAND = 64; // rain gutter on each edge
constexpr int COLS_SIDE = BAND / CELL_W;
constexpr int COLS = COLS_SIDE * 2;
constexpr int ROWS = H / CELL_H;

constexpr int TOTAL_STEPS = 6;
constexpr uint32_t MIN_DWELL_MS = 2100;

constexpr int BAR_X = 80, BAR_Y = 168, BAR_W = 160, BAR_H = 10;

const char CHARS[] = "01ABCDEFGHJKLMNPQRSTUVWXYZ<>*+#$%&/=|";
constexpr int NCHARS = sizeof(CHARS) - 1;

int8_t headY[COLS];
uint8_t speed[COLS];
uint8_t tailLen[COLS];
uint32_t frame = 0;
uint32_t t0 = 0;
int stepsDone = 0;
bool skipped = false;

int colX(int i) {
  return (i < COLS_SIDE) ? i * CELL_W : (W - BAND) + (i - COLS_SIDE) * CELL_W;
}

char randChar() { return CHARS[random(NCHARS)]; }

void cell(int col, int rowIdx, char ch, uint16_t fg) {
  if (rowIdx < 0 || rowIdx >= ROWS) return;
  ui::tft.setTextColor(fg, theme::c().bg);
  ui::tft.drawChar(ch, colX(col), rowIdx * CELL_H, 1);
}

void eraseCell(int col, int rowIdx) {
  if (rowIdx < 0 || rowIdx >= ROWS) return;
  ui::tft.fillRect(colX(col), rowIdx * CELL_H, CELL_W, CELL_H, theme::c().bg);
}

void resetCol(int i) {
  headY[i] = -(int8_t)random(ROWS);
  speed[i] = 1 + random(3);
  tailLen[i] = 6 + random(10);
}

void rainFrame() {
  const auto &p = theme::c();
  frame++;
  for (int i = 0; i < COLS; ++i) {
    if (frame % speed[i]) continue;
    const int y = headY[i];
    // Yesterday's head dims to a trailing glyph; a fresh character each time is
    // what gives the column its flicker.
    if (y > 0) cell(i, y - 1, randChar(), p.muted);
    if (y >= 0) cell(i, y, randChar(), p.fg);
    eraseCell(i, y - tailLen[i]);
    headY[i]++;
    if (headY[i] - tailLen[i] > ROWS) resetCol(i);
  }
}

// Letter-spaced wordmark, the way the PNG sets "SCOTTINA".
void spacedString(const char *s, int cy, int gap, uint8_t font, uint16_t color) {
  ui::tft.setTextFont(font);
  ui::tft.setTextDatum(TL_DATUM);
  int total = 0;
  for (const char *p = s; *p; ++p) {
    char b[2] = {*p, 0};
    total += ui::tft.textWidth(b, font) + gap;
  }
  total -= gap;
  int x = (W - total) / 2;
  ui::tft.setTextColor(color, theme::c().bg);
  for (const char *p = s; *p; ++p) {
    char b[2] = {*p, 0};
    ui::tft.drawString(b, x, cy);
    x += ui::tft.textWidth(b, font) + gap;
  }
}

void centered(const char *s, int y, uint8_t font, uint16_t color) {
  ui::tft.setTextFont(font);
  ui::tft.setTextDatum(TC_DATUM);
  ui::tft.setTextColor(color, theme::c().bg);
  ui::tft.drawString(s, W / 2, y);
  ui::tft.setTextDatum(TL_DATUM);
}

void drawBar() {
  const auto &p = theme::c();
  ui::tft.drawRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 3, p.muted);
  const int inner = BAR_W - 4;
  const int filled = (inner * stepsDone) / TOTAL_STEPS;
  ui::tft.fillRect(BAR_X + 2, BAR_Y + 2, filled, BAR_H - 4, p.accent);
  if (filled < inner) {
    ui::tft.fillRect(BAR_X + 2 + filled, BAR_Y + 2, inner - filled, BAR_H - 4, p.bg);
  }
}

void drawStatus(const char *label) {
  const auto &p = theme::c();
  ui::tft.fillRect(BAND, 186, W - BAND * 2, 10, p.bg);
  centered(label, 186, 1, p.muted);
}

} // namespace

void begin() {
  const auto &p = theme::c();
  randomSeed((uint32_t)analogRead(WIO_LIGHT) ^ micros());

  ui::tft.fillScreen(p.bg);
  for (int i = 0; i < COLS; ++i) resetCol(i);

  frame = 0;
  stepsDone = 0;
  skipped = false;
  t0 = millis();

  // Wordmark. Drawn twice, one pixel apart, to fake a bold weight the built-in
  // fonts do not carry.
  ui::tft.setTextFont(4);
  ui::tft.setTextDatum(TC_DATUM);
  ui::tft.setTextColor(p.fg, p.bg);
  ui::tft.drawString(SL_PRODUCT_MARK, W / 2, 12);
  ui::tft.drawString(SL_PRODUCT_MARK, W / 2 + 1, 12);
  ui::tft.setTextDatum(TL_DATUM);

  spacedString("LIGHT", 42, 5, 2, p.accent);

  centered("DIGITAL SWISS ARMY KNIFE", 64, 1, p.muted);
  centered("FOR HARDWARE DEVELOPERS", 74, 1, p.muted);

  pict::draw(pict::Glyph::Knife, W / 2, 120, 28, p.fg, p.bg);

  centered("INITIATING SYSTEM...", 152, 1, p.fg);
  drawBar();
  centered(SL_TAGLINE, 202, 1, p.fg);
  centered(SL_BYLINE " " SL_VERSION, 218, 1, p.muted);
}

void step(const char *label) {
  if (skipped) return;
  stepsDone++;
  if (stepsDone > TOTAL_STEPS) stepsDone = TOTAL_STEPS;
  drawBar();
  drawStatus(label);
  for (int i = 0; i < 5; ++i) {
    rainFrame();
    delay(12);
  }
}

void finish() {
  drawStatus("READY");
  while (!skipped && millis() - t0 < MIN_DWELL_MS) {
    if (input::poll() != Btn::None) skipped = true;
    rainFrame();
    delay(28);
  }
}

} // namespace splash
