#ifndef SL_THEME_H
#define SL_THEME_H

#include <Arduino.h>

// Palettes ported verbatim from the mother project (Scottina, kilodash/theme.py).
//
// Three CRT-inspired skins. Chrome is monochrome (one phosphor colour + its
// shades); the traffic-light status colours (ok/warn/bad) stay vivid across all
// three so warnings always read. Blue is kept low in green/amber so the phosphor
// reads true, not teal.

namespace theme {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

struct Palette {
  const char *name;
  uint16_t bg, card, card_hi, fg, muted, accent, ok, warn, bad, ink;
  bool sterile; // chrome is greyscale; only ok/warn/bad carry colour
};

const Palette &c();
uint8_t index();
uint8_t count();
void set(uint8_t i);
void setByName(const char *name);
uint8_t cycle();

} // namespace theme

#endif // SL_THEME_H
