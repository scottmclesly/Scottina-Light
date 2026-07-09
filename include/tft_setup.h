// TFT_eSPI (Seeed_Arduino_LCD) configuration for the Seeed Wio Terminal.
//
// The library's User_Setup_Select.h defaults to Setup666_XIAO_ILI9341 and leaves
// the Wio Terminal setup commented out, so the stock config drives the wrong pins
// and the panel stays dark. This header is force-included ahead of every
// translation unit (see -include in platformio.ini) and defines USER_SETUP_LOADED,
// which makes User_Setup_Select.h skip its own selection entirely.
//
// Keep this in the project rather than editing .pio/libdeps -- that directory is
// regenerated on a clean build and any edit there is silently lost.
//
// Values mirror User_Setups/Setup500_Seeed_Wio_Terminal.h. The LCD_* macros come
// from the board variant; they expand lazily at point of use, after variant.h has
// been pulled in by Arduino.h, so referencing them here is safe.

#ifndef SL_TFT_SETUP_H
#define SL_TFT_SETUP_H

#define USER_SETUP_LOADED 1
#define USER_SETUP_ID 500

#define ILI9341_DRIVER

#define TFT_SPI_PORT LCD_SPI
#define TFT_CS LCD_SS_PIN
#define TFT_DC LCD_DC
#define TFT_BL LCD_BACKLIGHT
#define TFT_BACKLIGHT_ON HIGH
#define TFT_RST LCD_RESET

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT

#define SPI_FREQUENCY 50000000
#define SPI_READ_FREQUENCY 20000000

#endif // SL_TFT_SETUP_H
