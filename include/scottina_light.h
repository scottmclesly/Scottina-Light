#ifndef SCOTTINA_LIGHT_H
#define SCOTTINA_LIGHT_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>

#include "pictograms.h"
#include "scope.h"
#include "theme.h"

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

enum class Btn : uint8_t { None = 0, A, B, C, Up, Down, Left, Right, Press };

const char *btnName(Btn b);

namespace input {
void begin();
// Returns one press event per call, Btn::None when idle. Edge-triggered,
// debounced. Also drains the serial test hook when built with SL_TEST_HOOK.
Btn poll();
} // namespace input

// ---------------------------------------------------------------------------
// Screen framework  (TODO §0: enter/tick/exit/onButton, launcher switches screens)
// ---------------------------------------------------------------------------

class Screen {
public:
  virtual ~Screen() = default;
  virtual const char *title() const = 0;
  virtual void enter() {}
  virtual void tick(uint32_t nowMs) { (void)nowMs; }
  virtual void exit() {}
  virtual void onButton(Btn b) { (void)b; }
};

namespace ui {
extern TFT_eSPI tft;

constexpr int HEADER_H = 34; // scaled from the mother's 44px on a 480x320 panel
constexpr int FOOTER_H = 13;

void begin();
// Mother-style chrome: back chevron on the left, screen title right-aligned.
void header(const char *title, const char *hint);
void clearBody();
int bodyTop();
void note(int row, const char *text, uint16_t color);
void row(int idx, const char *text, uint16_t color, bool selected);

void setRoot(Screen *s);
void push(Screen *s);
void pop();
Screen *current();
bool isRoot(Screen *s);
void tick(uint32_t nowMs);
void dispatch(Btn b);
void repaint();
} // namespace ui

// ---------------------------------------------------------------------------
// Splash  (mirrors ScottinaSplash.png: phosphor rain, wordmark, boot progress)
// ---------------------------------------------------------------------------

namespace splash {
void begin();
void step(const char *label); // advance the progress bar, animate the rain
void finish();                // hold for the minimum dwell, or until a keypress
} // namespace splash

// ---------------------------------------------------------------------------
// I2C bus + device registry  (TODO §0 autodetect, §2 scanner)
// ---------------------------------------------------------------------------

namespace i2cbus {

constexpr uint8_t MAX_DEV = 16;

// begin() plus the phantom-ACK mitigation. On a bus with no pull-ups (nothing
// plugged into Grove), the SAMD51 SERCOM reports a spurious ACK for whichever
// address is probed first after a cold begin(). Verified on hardware: a cold
// probe of 0x42 ACKs; every probe after it correctly NACKs. Burning one
// throwaway transaction absorbs that false positive.
void beginBus(TwoWire &w);

bool probe(TwoWire &w, uint8_t addr);
bool readReg(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t &val);

// Two independent passes; an address must ACK on both to be reported.
uint8_t scan(TwoWire &w, uint8_t *out, uint8_t maxOut);

// NB: no enumerator may be named RTC or ADC -- CMSIS defines those as object-like
// macros (samd51p19a.h), and the preprocessor does not respect enum scope.
enum class Cap : uint8_t { Unknown = 0, Accel, TempHum, Baro, Light, Distance, RtcClock, Adc, Imu6, Current };

struct KnownDev {
  uint8_t addr;
  const char *name;
  Cap cap;
  bool (*verify)(TwoWire &w, uint8_t addr);
};

extern const KnownDev KNOWN[];
extern const uint8_t KNOWN_N;

uint8_t candidatesFor(uint8_t addr, const KnownDev **out, uint8_t maxOut);
const KnownDev *autoResolve(TwoWire &w, uint8_t addr);
const char *capName(Cap c);

} // namespace i2cbus

// ---------------------------------------------------------------------------
// Detected-hardware inventory  (drives which tiles exist, TODO §0)
// ---------------------------------------------------------------------------

struct Detected {
  uint8_t addr;
  TwoWire *bus;
  const char *busName;
  const char *name;
  i2cbus::Cap cap;
  bool ambiguous;
};

namespace inventory {
constexpr uint8_t MAX = 24;
extern Detected devs[MAX];
extern uint8_t count;

void rescan();
bool hasCap(i2cbus::Cap c);
Detected *findByAddr(uint8_t addr);
} // namespace inventory

// ---------------------------------------------------------------------------
// Storage  (TODO §3)
// ---------------------------------------------------------------------------

namespace storage {
bool begin();
bool mounted();
bool cardPresent();
// Hot-plug reconciliation: mounts an inserted card, drops a removed one.
// Returns true when the mounted state changed.
bool refresh();
uint32_t totalMB();
uint32_t freeMB();
bool ensureDir(const char *path);
} // namespace storage

// ---------------------------------------------------------------------------
// Config  (TODO §1: three tiers)
// ---------------------------------------------------------------------------

namespace config {

struct Tier3 {
  char wifiSsid[33];
  char wifiPass[65];
  uint32_t canBitrate;
  uint32_t uartBaud;
  uint8_t mcp2515CsPin;
  uint32_t logMaxBytes;
  uint32_t minFreeMB;
};

extern Tier3 t3;
extern bool loadedFromFile;

bool load();

// Tier 2 -- on-device picks, persisted so the user is asked once.
bool saveChoice(uint8_t addr, const char *name);
bool loadChoice(uint8_t addr, char *out, size_t outLen);
bool saveTheme(const char *name);

} // namespace config

// ---------------------------------------------------------------------------
// Raw logger  (TODO §3)
// ---------------------------------------------------------------------------

namespace logger {
bool open(const char *prefix);
bool isOpen();
void writeLine(const char *s);
void writeRaw(const uint8_t *data, size_t n);
void flush();
void close();
uint32_t bytesWritten();
const char *path();
bool spaceExhausted();
} // namespace logger

// ---------------------------------------------------------------------------
// CAN front-end presence  (TODO §4; Grove MCP2515 over the header SPI)
// ---------------------------------------------------------------------------

namespace canfe {
bool detect(uint8_t csPin);
bool present(); // last probe result, refreshed on the launcher tick
} // namespace canfe

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

Screen *launcherScreen();
Screen *i2cScreen();
Screen *imuScreen();
Screen *uartScreen();
Screen *canScreen();
Screen *logBrowserScreen();

void screensInit();
// Re-evaluates which tiles exist. Cheap; called on the launcher tick so the
// grid tracks hot-plugged hardware (TODO §0).
void refreshAvailability();

#endif // SCOTTINA_LIGHT_H
