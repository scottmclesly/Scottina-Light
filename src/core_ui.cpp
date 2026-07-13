#include "scottina_light.h"

#include "dock.h"

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

namespace {

struct BtnPin {
  uint8_t pin;
  Btn btn;
};

const BtnPin BTN_PINS[] = {
    {WIO_KEY_A, Btn::A},        {WIO_KEY_B, Btn::B},
    {WIO_KEY_C, Btn::C},        {WIO_5S_UP, Btn::Up},
    {WIO_5S_DOWN, Btn::Down},   {WIO_5S_LEFT, Btn::Left},
    {WIO_5S_RIGHT, Btn::Right}, {WIO_5S_PRESS, Btn::Press},
};
constexpr uint8_t BTN_N = sizeof(BTN_PINS) / sizeof(BTN_PINS[0]);

bool lastLevel[BTN_N];
uint32_t lastChange[BTN_N];
constexpr uint32_t DEBOUNCE_MS = 25;

} // namespace

const char *btnName(Btn b) {
  switch (b) {
  case Btn::A: return "A";
  case Btn::B: return "B";
  case Btn::C: return "C";
  case Btn::Up: return "UP";
  case Btn::Down: return "DOWN";
  case Btn::Left: return "LEFT";
  case Btn::Right: return "RIGHT";
  case Btn::Press: return "PRESS";
  default: return "-";
  }
}

namespace input {

void begin() {
  for (uint8_t i = 0; i < BTN_N; ++i) {
    pinMode(BTN_PINS[i].pin, INPUT_PULLUP);
    lastLevel[i] = true;
    lastChange[i] = 0;
  }
}

Btn poll() {
#ifdef SL_TEST_HOOK
  // Drive the UI over serial so navigation can be exercised without hands on
  // the device. Diagnostics build only.
  //
  // Once the dock owns the port, this reader must not touch Serial AT ALL. Note
  // what it does below with a byte it does not recognise: `default: break` --
  // it consumes the byte and drops it on the floor. Pointed at a frame stream
  // that behaviour is not a collision, it is a shredder. main.cpp calls
  // dock::tick() before us so frames are drained first; this guard is the
  // second lock on the same door.
  if (!dock::active() && Serial.available()) {
    switch (Serial.read()) {
    case 'a': return Btn::A;
    case 'b': return Btn::B;
    case 'c': return Btn::C;
    case 'u': return Btn::Up;
    case 'd': return Btn::Down;
    case 'l': return Btn::Left;
    case 'r': return Btn::Right;
    case 'p': return Btn::Press;
    default: break;
    }
  }
#endif

  const uint32_t now = millis();
  for (uint8_t i = 0; i < BTN_N; ++i) {
    const bool level = digitalRead(BTN_PINS[i].pin) != 0;
    if (level == lastLevel[i]) continue;
    if (now - lastChange[i] < DEBOUNCE_MS) continue;
    lastChange[i] = now;
    lastLevel[i] = level;
    if (!level) return BTN_PINS[i].btn; // active low: falling edge is a press
  }
  return Btn::None;
}

} // namespace input

// ---------------------------------------------------------------------------
// UI / screen stack
// ---------------------------------------------------------------------------

namespace ui {

TFT_eSPI tft;

namespace {
constexpr uint8_t STACK_MAX = 4;
Screen *stack[STACK_MAX];
uint8_t depth = 0;
} // namespace

void begin() {
  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(theme::c().bg);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
}

int bodyTop() { return HEADER_H + 5; }

// Detail-screen chrome, mirroring the mother's base.Screen._draw_header:
// back affordance on the left in accent, screen title right-aligned in fg.
void header(const char *title, const char *hint) {
  const auto &p = theme::c();
  tft.fillRect(0, 0, tft.width(), HEADER_H, p.card);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(p.accent, p.card);
  tft.setTextFont(4);
  tft.drawString("<", 8, 3);
  tft.setTextFont(2);
  tft.drawString("Back", 24, 8);

  tft.setTextFont(2);
  tft.setTextColor(p.fg, p.card);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(title, tft.width() - 10, 8);
  tft.setTextDatum(TL_DATUM);

  if (hint) {
    tft.fillRect(0, tft.height() - FOOTER_H, tft.width(), FOOTER_H, p.bg);
    tft.setTextFont(1);
    tft.setTextColor(p.muted, p.bg);
    tft.drawString(hint, 6, tft.height() - FOOTER_H + 3);
  }
}

void clearBody() {
  const auto &p = theme::c();
  tft.fillRect(0, HEADER_H, tft.width(), tft.height() - HEADER_H - FOOTER_H, p.bg);
}

void note(int rowIdx, const char *text, uint16_t color) {
  const auto &p = theme::c();
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, p.bg);
  const int y = bodyTop() + rowIdx * 12;
  tft.fillRect(0, y, tft.width(), 12, p.bg);
  tft.drawString(text, 6, y);
}

void row(int idx, const char *text, uint16_t color, bool selected) {
  const auto &p = theme::c();
  const int y = bodyTop() + idx * 14;
  const uint16_t bg = selected ? p.card_hi : p.bg;
  tft.fillRect(0, y, tft.width(), 14, bg);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, bg);
  tft.drawString(text, 8, y + 3);
}

Screen *current() { return depth ? stack[depth - 1] : nullptr; }
bool isRoot(Screen *s) { return depth && stack[0] == s; }

static void enterTop() {
  Screen *s = current();
  if (!s) return;
  tft.fillScreen(theme::c().bg);
  s->enter();
  Serial.print("NAV screen=");
  Serial.println(s->title());
}

void repaint() { enterTop(); }

void setRoot(Screen *s) {
  while (depth) stack[--depth]->exit();
  stack[depth++] = s;
  enterTop();
}

void push(Screen *s) {
  if (depth >= STACK_MAX) return;
  if (current()) current()->exit();
  stack[depth++] = s;
  enterTop();
}

void pop() {
  if (depth <= 1) return; // never pop the launcher
  stack[--depth]->exit();
  enterTop();
}

void tick(uint32_t nowMs) {
  Screen *s = current();
  if (s) s->tick(nowMs);
}

void dispatch(Btn b) {
  Screen *s = current();
  if (s) s->onButton(b);
}

} // namespace ui
