// Scottina Light -- standalone diagnostics instrument, Wio Terminal sibling of
// the Scottina front panel. Read include/scope.h before adding anything.

#include "scottina_light.h"

#include "SD/Seeed_SD.h"
#include "Seeed_FS.h"

namespace {

uint8_t g_checks = 0;
uint8_t g_fails = 0;

void check(const char *name, bool ok, const char *detail) {
  g_checks++;
  if (!ok) g_fails++;
  Serial.print("SMOKE ");
  Serial.print(name);
  Serial.print('=');
  Serial.print(ok ? "PASS" : "FAIL");
  if (detail && *detail) {
    Serial.print("  ");
    Serial.print(detail);
  }
  Serial.println();
}

void info(const char *name, const char *detail) {
  Serial.print("SMOKE ");
  Serial.print(name);
  Serial.print("=INFO  ");
  Serial.println(detail);
}

// A capture round-trip: create, write, close, then stat the file back.
bool loggerRoundTrip(char *detail, size_t n) {
  if (!storage::mounted()) {
    snprintf(detail, n, "skipped, no SD");
    return false;
  }
  if (!logger::open("smoke")) {
    snprintf(detail, n, "open failed");
    return false;
  }
  char path[48];
  strncpy(path, logger::path(), sizeof(path) - 1);
  path[sizeof(path) - 1] = '\0';

  for (int i = 0; i < 8; ++i) logger::writeLine("# scottina light smoke record");
  const uint32_t wrote = logger::bytesWritten();
  logger::close();

  if (!SD.exists(path)) {
    snprintf(detail, n, "%s vanished after close", path);
    return false;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    snprintf(detail, n, "%s unreadable", path);
    return false;
  }
  const uint32_t sz = f.size();
  f.close();

  // Clean up after ourselves: this runs on every boot, and leaving the probe
  // file behind would slowly pack /logs with smokeNNN.log.
  const bool removed = SD.remove(path);

  snprintf(detail, n, "%s wrote=%lu read=%lu cleanup=%s", path,
           (unsigned long)wrote, (unsigned long)sz, removed ? "ok" : "FAILED");
  return sz > 0 && sz == wrote && removed;
}

void selfTest() {
  Serial.println("SMOKE begin " SL_PRODUCT " " SL_VERSION);

  char d[80];
  const bool lcdOk = ui::tft.width() == 320 && ui::tft.height() == 240;
  snprintf(d, sizeof(d), "%dx%d", ui::tft.width(), ui::tft.height());
  check("lcd", lcdOk, d);

  // --- I2C: the internal accelerometer must be present and self-identify ---
  uint8_t who = 0;
  const bool imuOk = i2cbus::readReg(Wire1, 0x18, 0x0F, who) && who == 0x33;
  snprintf(d, sizeof(d), "Wire1 0x18 WHO_AM_I=0x%02X", who);
  check("i2c.imu", imuOk, d);

  // --- I2C: the cold-begin phantom must not have been reported ---
  uint8_t groveN = 0, internalN = 0;
  for (uint8_t i = 0; i < inventory::count; ++i) {
    if (strcmp(inventory::devs[i].busName, "grove") == 0) groveN++;
    else internalN++;
  }
  if (groveN == 0) {
    check("i2c.phantom", true, "grove bus clean (0x08 artifact absorbed)");
  } else {
    snprintf(d, sizeof(d), "grove has %u real device(s); phantom check n/a", groveN);
    info("i2c.phantom", d);
  }
  snprintf(d, sizeof(d), "grove=%u internal=%u total=%u", groveN, internalN,
           inventory::count);
  check("i2c.scan", internalN >= 1, d);

  bool reservedClean = true;
  for (uint8_t i = 0; i < inventory::count; ++i) {
    const uint8_t a = inventory::devs[i].addr;
    if (a < 0x08 || a > 0x77) reservedClean = false;
  }
  check("i2c.reserved", reservedClean, "all addrs in 0x08..0x77");

  snprintf(d, sizeof(d), "present=%s mounted=%s total=%luMB free=%luMB",
           storage::cardPresent() ? "yes" : "no",
           storage::mounted() ? "yes" : "no",
           (unsigned long)storage::totalMB(), (unsigned long)storage::freeMB());
  check("sd", storage::mounted(), d);

  snprintf(d, sizeof(d), "source=%s can_bitrate=%lu uart_baud=%lu cs=%u",
           config::loadedFromFile ? "config.json" : "defaults",
           (unsigned long)config::t3.canBitrate,
           (unsigned long)config::t3.uartBaud, config::t3.mcp2515CsPin);
  check("config", config::t3.mcp2515CsPin != 0, d);

  char ld[80];
  const bool logOk = loggerRoundTrip(ld, sizeof(ld));
  check("logger", logOk, ld);

  snprintf(d, sizeof(d), "mcp2515=%s cs=%u", canfe::present() ? "present" : "absent",
           config::t3.mcp2515CsPin);
  info("can", d);

  snprintf(d, sizeof(d), "theme=%s root=%s", theme::c().name,
           ui::current() ? ui::current()->title() : "none");
  info("ui", d);

  Serial.print("SMOKE result=");
  Serial.print(g_fails == 0 ? "PASS" : "FAIL");
  Serial.print(" checks=");
  Serial.print(g_checks);
  Serial.print(" failures=");
  Serial.println(g_fails);
  Serial.println("SMOKE end");
}

} // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {} // never block a headless boot

  Serial.println();
  Serial.println("=== " SL_PRODUCT " " SL_VERSION " ===");
  Serial.println("diagnostics only -- see include/scope.h");

  ui::begin();

  // Mount and read config before the splash so it paints in the user's chosen
  // phosphor rather than flipping palette a moment later.
  storage::begin();
  config::load();

  splash::begin();

  splash::step("input");
  input::begin();

  splash::step("i2c bus");
  // beginBus() absorbs the cold-begin phantom ACK before anything may scan.
  i2cbus::beginBus(Wire);
  i2cbus::beginBus(Wire1);

  splash::step("bus scan");
  inventory::rescan();

  splash::step("can front end");
  splash::step("screens");
  screensInit();

  splash::step("ready");
  splash::finish();

  ui::setRoot(launcherScreen());
  selfTest();

#ifdef SL_TEST_HOOK
  Serial.println("HOOK keys: a b c u d l r p  (buttons / 5-way)");
#endif
}

void loop() {
  const Btn b = input::poll();
  if (b != Btn::None) {
    Serial.print("BTN ");
    Serial.println(btnName(b));
    ui::dispatch(b);
  }
  ui::tick(millis());
}
