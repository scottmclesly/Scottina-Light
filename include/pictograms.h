#ifndef SL_PICTOGRAMS_H
#define SL_PICTOGRAMS_H

#include <Arduino.h>

// Semiotic-standard-inspired pictograms for the launcher tiles, ported from the
// mother project (Scottina, kilodash/pictograms.py).
//
// Bold, geometric, monochrome glyphs in the spirit of Ron Cobb's "Semiotic
// Standard" (the Alien corridor icons): circles, triangles, bars and hard
// diagonals -- one symbol per subsystem, readable at arm's length.
//
// I2c/Can/Serial/Settings reproduce the mother's geometry exactly. Vibration,
// Sound, Light and Logs are new, drawn in the same language because the Pi has
// no equivalent subsystem. Knife is the splash multi-tool.
//
// Sound and Light are the two sibling loggers of Vibration and are deliberately
// built from the SAME primitives at the same weight, so the three read as one
// family on the launcher grid rather than three borrowed icons.
//
// All glyphs draw centred on (cx, cy) inside radius r, single colour. `bg` is
// the surface being drawn onto -- the anti-aliased primitives blend against it.

namespace pict {

enum class Glyph : uint8_t {
  None = 0,
  I2c,
  Can,
  Serial,
  Vibration,
  Sound,
  Light,
  Logs,
  Settings,
  Knife
};

void draw(Glyph g, int cx, int cy, int r, uint16_t color, uint16_t bg);

} // namespace pict

#endif // SL_PICTOGRAMS_H
