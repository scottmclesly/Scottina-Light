#include "theme.h"

namespace theme {

namespace {

using R = uint16_t;

const Palette PALETTES[] = {
    // Classic green phosphor / "Matrix": bright #33F546, dim #009628 on black.
    {"green",
     rgb565(0, 9, 3),      // bg
     rgb565(3, 26, 10),    // card
     rgb565(8, 46, 18),    // card_hi
     rgb565(51, 245, 70),  // fg
     rgb565(0, 150, 40),   // muted
     rgb565(130, 255, 120),// accent
     rgb565(51, 235, 80),  // ok
     rgb565(255, 190, 40), // warn
     rgb565(255, 75, 60),  // bad
     rgb565(0, 9, 3),      // ink
     false},
    // P3 amber phosphor / Pip-Boy: #FFB642 on warm black.
    {"amber",
     rgb565(12, 6, 0),
     rgb565(32, 19, 2),
     rgb565(52, 33, 6),
     rgb565(255, 182, 66),
     rgb565(170, 112, 28),
     rgb565(255, 214, 110),
     rgb565(150, 225, 90),
     rgb565(255, 226, 90),
     rgb565(255, 96, 66),
     rgb565(12, 6, 0),
     false},
    // Sterile clinical white. Chrome is greyscale (accent is a neutral dark
    // slate, not a colour); only ok/warn/bad carry colour, for meaning alone.
    {"light",
     rgb565(238, 240, 243),
     rgb565(255, 255, 255),
     rgb565(222, 226, 231),
     rgb565(22, 26, 32),
     rgb565(120, 130, 142),
     rgb565(44, 50, 60),
     rgb565(22, 160, 82),
     rgb565(200, 140, 0),
     rgb565(208, 55, 48),
     rgb565(255, 255, 255),
     true},
};

constexpr uint8_t N = sizeof(PALETTES) / sizeof(PALETTES[0]);
uint8_t s_idx = 0;

} // namespace

const Palette &c() { return PALETTES[s_idx]; }
uint8_t index() { return s_idx; }
uint8_t count() { return N; }

void set(uint8_t i) {
  if (i < N) s_idx = i;
}

void setByName(const char *name) {
  if (!name) return;
  for (uint8_t i = 0; i < N; ++i) {
    if (strcmp(PALETTES[i].name, name) == 0) {
      s_idx = i;
      return;
    }
  }
}

uint8_t cycle() {
  s_idx = (uint8_t)((s_idx + 1) % N);
  return s_idx;
}

} // namespace theme
