#include "scottina_light.h"

#include <ArduinoJson.h>
#include <SPI.h>

#include "SD/Seeed_SD.h"
#include "Seeed_FS.h"

// The Seeed FS layer permits only one open file at a time. Everything below is
// written around that: the logger owns the single handle while a capture runs,
// and config writes refuse rather than yank it away mid-capture.

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

namespace storage {

namespace {
bool s_mounted = false;
}

bool cardPresent() {
  pinMode(SDCARD_DET_PIN, INPUT_PULLUP);
  return digitalRead(SDCARD_DET_PIN) == LOW; // verified: LOW with card seated
}

bool begin() {
  s_mounted = SD.begin(SDCARD_SS_PIN, SDCARD_SPI, 8000000);
  if (s_mounted) {
    ensureDir("/logs");
    ensureDir("/config");
    // The dock pushes decode tables here. Nothing on Light READS them yet -- the
    // decode layer is not written -- so the dock deliberately provisions ahead
    // of its consumer. A successful table push with no visible effect is correct.
    ensureDir("/tables");
    sweepPartials();
  }
  return s_mounted;
}

bool mounted() { return s_mounted; }

bool refresh() {
  const bool card = cardPresent();
  if (card && !s_mounted) return begin();
  if (!card && s_mounted) {
    if (logger::isOpen()) logger::close();
    SD.end();
    s_mounted = false;
    Serial.println("[sd ] card removed");
    return true;
  }
  return false;
}

uint32_t totalMB() {
  if (!s_mounted) return 0;
  return (uint32_t)(SD.totalBytes() / (1024ULL * 1024ULL));
}

uint32_t freeMB() {
  if (!s_mounted) return 0;
  const uint64_t total = SD.totalBytes();
  const uint64_t used = SD.usedBytes();
  if (used > total) return 0;
  return (uint32_t)((total - used) / (1024ULL * 1024ULL));
}

bool ensureDir(const char *path) {
  if (!s_mounted) return false;
  if (SD.exists(path)) return true;
  return SD.mkdir(path);
}

uint8_t sweepPartials() {
  if (!s_mounted) return 0;

  // Only /tables/ can hold staging files -- it is the sole writable directory
  // the dock exposes besides /config.json, whose .partial sits at the root.
  const char *const dirs[] = {"/tables", "/"};
  uint8_t swept = 0;

  for (uint8_t d = 0; d < 2; ++d) {
    // Collect first, unlink after: the one-open-file rule means we cannot hold
    // the directory handle open while removing entries out from under it.
    char doomed[8][64];
    uint8_t n = 0;

    File dir = SD.open(dirs[d]);
    if (!dir) continue;
    for (File f = dir.openNextFile(); f && n < 8; f = dir.openNextFile()) {
      const char *nm = f.name();
      const size_t len = nm ? strlen(nm) : 0;
      const bool isPartial = len > 8 && strcmp(nm + len - 8, ".partial") == 0;
      if (isPartial && !f.isDirectory()) {
        snprintf(doomed[n], sizeof(doomed[n]), "%s%s%s", dirs[d],
                 (strcmp(dirs[d], "/") == 0) ? "" : "/", nm);
        n++;
      }
      f.close();
    }
    dir.close();

    for (uint8_t i = 0; i < n; ++i) {
      if (SD.remove(doomed[i])) {
        Serial.print("[dock] swept stale staging file ");
        Serial.println(doomed[i]);
        swept++;
      }
    }
  }
  return swept;
}

} // namespace storage

// ---------------------------------------------------------------------------
// Wall clock
// ---------------------------------------------------------------------------

namespace wallclock {

namespace {
uint64_t s_baseEpoch = 0; // epoch at the moment SET_CLOCK landed
uint32_t s_baseMs = 0;    // millis() at that same moment
uint8_t s_quality = 0;
bool s_set = false;
} // namespace

void set(uint64_t epoch, uint8_t quality) {
  s_baseEpoch = epoch;
  s_baseMs = millis();
  s_quality = quality;
  s_set = true;
}

uint64_t now() {
  if (!s_set) return 0;
  // millis() wraps at ~49 days. Unsigned subtraction wraps with it, so the
  // delta stays correct across a single wrap -- which is far longer than any
  // capture this instrument is designed for.
  const uint32_t delta = millis() - s_baseMs;
  return s_baseEpoch + (uint64_t)(delta / 1000UL);
}

uint8_t quality() { return s_quality; }
bool setThisBoot() { return s_set; }

const char *qualityName() {
  switch (s_quality) {
  case 1: return "rtc";
  case 2: return "ntp";
  default: return "unsynced";
  }
}

} // namespace wallclock

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

namespace config {

Tier3 t3;
bool loadedFromFile = false;

namespace {

struct Choice {
  uint8_t addr;
  char name[40];
};
constexpr uint8_t CHOICE_MAX = 8;
Choice s_choices[CHOICE_MAX];
uint8_t s_choiceN = 0;

constexpr const char *CHOICE_PATH = "/config/i2c.json";
constexpr const char *UI_PATH = "/config/ui.json";
constexpr const char *CONFIG_PATH = "/config.json";

void applyDefaults() {
  t3.wifiSsid[0] = '\0';
  t3.wifiPass[0] = '\0';
  t3.canBitrate = 0;   // autobaud
  t3.uartBaud = 0;     // autobaud
  t3.mcp2515CsPin = PIN_SPI_SS;
  t3.logMaxBytes = 8UL * 1024UL * 1024UL;
  t3.minFreeMB = 32;
}

void copyStr(char *dst, size_t cap, const char *src) {
  if (!src) return;
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// Choices live in RAM so that a bus scan never needs to open a file (the
// one-handle limit would collide with an in-flight capture).
void loadChoices() {
  s_choiceN = 0;
  if (!storage::mounted() || !SD.exists(CHOICE_PATH)) return;
  File f = SD.open(CHOICE_PATH, FILE_READ);
  if (!f) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("[cfg] i2c.json malformed, ignoring: ");
    Serial.println(err.c_str());
    return;
  }
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (s_choiceN >= CHOICE_MAX) break;
    s_choices[s_choiceN].addr = (uint8_t)strtol(kv.key().c_str(), nullptr, 16);
    copyStr(s_choices[s_choiceN].name, sizeof(s_choices[s_choiceN].name),
            kv.value().as<const char *>());
    s_choiceN++;
  }
}

void loadTheme() {
  if (!storage::mounted() || !SD.exists(UI_PATH)) return;
  File f = SD.open(UI_PATH, FILE_READ);
  if (!f) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  const char *name = doc["theme"];
  if (name) theme::setByName(name);
}

// Tier-2 state: on-device picks the user made, reloaded at boot.
void loadTier2() {
  loadChoices();
  loadTheme();
}

bool writeChoices() {
  if (!storage::mounted()) return false;
  JsonDocument doc;
  for (uint8_t i = 0; i < s_choiceN; ++i) {
    char key[6];
    snprintf(key, sizeof(key), "0x%02X", s_choices[i].addr);
    doc[key] = s_choices[i].name;
  }
  File f = SD.open(CHOICE_PATH, FILE_WRITE);
  if (!f) return false;
  const bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

} // namespace

bool load() {
  applyDefaults();
  loadedFromFile = false;

  if (!storage::mounted()) {
    Serial.println("[cfg] no SD -> built-in defaults");
    loadTier2();
    return false;
  }
  if (!SD.exists(CONFIG_PATH)) {
    Serial.println("[cfg] /config.json absent -> built-in defaults");
    loadTier2();
    return false;
  }

  File f = SD.open(CONFIG_PATH, FILE_READ);
  if (!f) {
    Serial.println("[cfg] /config.json unreadable -> built-in defaults");
    loadTier2();
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    // Never hard-fail on bad config (TODO §1).
    Serial.print("[cfg] /config.json malformed -> defaults: ");
    Serial.println(err.c_str());
    loadTier2();
    return false;
  }

  copyStr(t3.wifiSsid, sizeof(t3.wifiSsid), doc["wifi"]["ssid"] | "");
  copyStr(t3.wifiPass, sizeof(t3.wifiPass), doc["wifi"]["pass"] | "");
  t3.canBitrate = doc["can"]["bitrate"] | t3.canBitrate;
  t3.uartBaud = doc["uart"]["baud"] | t3.uartBaud;
  t3.mcp2515CsPin = doc["can"]["cs_pin"] | t3.mcp2515CsPin;
  t3.logMaxBytes = doc["log"]["max_bytes"] | t3.logMaxBytes;
  t3.minFreeMB = doc["log"]["min_free_mb"] | t3.minFreeMB;

  loadedFromFile = true;
  Serial.println("[cfg] /config.json loaded");
  loadTier2();
  return true;
}

bool loadChoice(uint8_t addr, char *out, size_t outLen) {
  for (uint8_t i = 0; i < s_choiceN; ++i) {
    if (s_choices[i].addr == addr) {
      strncpy(out, s_choices[i].name, outLen - 1);
      out[outLen - 1] = '\0';
      return true;
    }
  }
  return false;
}

bool saveChoice(uint8_t addr, const char *name) {
  if (!storage::mounted()) return false;
  // Refuse rather than steal the single file handle out from under a capture.
  if (logger::isOpen()) return false;

  for (uint8_t i = 0; i < s_choiceN; ++i) {
    if (s_choices[i].addr == addr) {
      copyStr(s_choices[i].name, sizeof(s_choices[i].name), name);
      return writeChoices();
    }
  }
  if (s_choiceN >= CHOICE_MAX) return false;
  s_choices[s_choiceN].addr = addr;
  copyStr(s_choices[s_choiceN].name, sizeof(s_choices[s_choiceN].name), name);
  s_choiceN++;
  return writeChoices();
}

bool saveTheme(const char *name) {
  if (!storage::mounted() || logger::isOpen()) return false;
  JsonDocument doc;
  doc["theme"] = name;
  File f = SD.open(UI_PATH, FILE_WRITE);
  if (!f) return false;
  const bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

} // namespace config

// ---------------------------------------------------------------------------
// Raw logger
// ---------------------------------------------------------------------------

namespace logger {

namespace {
File s_file;
bool s_open = false;
uint32_t s_bytes = 0;
char s_path[48];
char s_prefix[16];
uint16_t s_index = 0;
bool s_exhausted = false;

bool openIndexed(const char *prefix, uint16_t idx) {
  snprintf(s_path, sizeof(s_path), "/logs/%s%03u.log", prefix, idx);
  s_file = SD.open(s_path, FILE_WRITE);
  return (bool)s_file;
}

// Roll to the next file rather than growing without bound (TODO §3).
void rotate() {
  s_file.close();
  s_index++;
  if (!openIndexed(s_prefix, s_index)) {
    s_open = false;
    return;
  }
  s_bytes = 0;
  Serial.print("[log] rotated -> ");
  Serial.println(s_path);
}

void guardSpace() {
  if (storage::freeMB() < config::t3.minFreeMB) {
    s_exhausted = true;
    Serial.println("[log] free space below floor -> capture stopped");
    close();
  }
}
} // namespace

bool open(const char *prefix) {
  if (s_open) return true;
  if (!storage::mounted()) return false;
  if (storage::freeMB() < config::t3.minFreeMB) {
    s_exhausted = true;
    return false;
  }

  strncpy(s_prefix, prefix, sizeof(s_prefix) - 1);
  s_prefix[sizeof(s_prefix) - 1] = '\0';

  // Pick the first unused index so a capture never clobbers an earlier one.
  for (s_index = 0; s_index < 1000; ++s_index) {
    char probe[48];
    snprintf(probe, sizeof(probe), "/logs/%s%03u.log", s_prefix, s_index);
    if (!SD.exists(probe)) break;
  }
  if (!openIndexed(s_prefix, s_index)) return false;

  s_open = true;
  s_bytes = 0;
  s_exhausted = false;
  Serial.print("[log] open ");
  Serial.println(s_path);
  return true;
}

bool isOpen() { return s_open; }

void writeRaw(const uint8_t *data, size_t n) {
  if (!s_open) return;
  s_bytes += s_file.write(data, n);
  if (s_bytes >= config::t3.logMaxBytes) {
    rotate();
    guardSpace();
  }
}

void writeLine(const char *s) {
  if (!s_open) return;
  s_bytes += s_file.write((const uint8_t *)s, strlen(s));
  const uint8_t nl = '\n';
  s_bytes += s_file.write(&nl, 1);
  if (s_bytes >= config::t3.logMaxBytes) {
    rotate();
    guardSpace();
  }
}

void flush() {
  if (s_open) s_file.flush();
}

void close() {
  if (!s_open) return;
  s_file.flush();
  s_file.close();
  s_open = false;
  Serial.print("[log] closed ");
  Serial.println(s_path);
}

uint32_t bytesWritten() { return s_bytes; }
const char *path() { return s_path; }
bool spaceExhausted() { return s_exhausted; }

// --- dock quiesce -----------------------------------------------------------

namespace {
bool s_dockSuspended = false;
uint32_t s_suspendedAtMs = 0;
} // namespace

void suspendForDock() {
  if (s_dockSuspended) return;
  s_dockSuspended = true;
  s_suspendedAtMs = millis();

  // Only meaningful if a capture was actually running. If it wasn't, we still
  // latch the flag so resume() knows not to invent a file that never existed.
  if (!s_open) return;

  close(); // hands the single file handle to the dock
  Serial.println("[dock] logging suspended -- the dock needs the file handle");
}

void resumeAfterDock() {
  if (!s_dockSuspended) return;
  const uint32_t gapMs = millis() - s_suspendedAtMs;
  s_dockSuspended = false;

  // s_prefix is empty if no capture was ever running; nothing to resume.
  if (s_prefix[0] == '\0') return;
  if (!storage::mounted()) return;

  if (!open(s_prefix)) {
    Serial.println("[dock] WARNING: could not resume logging after dock");
    return;
  }

  // The gap goes INTO the record. Forensics must be able to see that these
  // bytes were written after a dock, and what clock they are being stamped
  // with -- a clock Prime may itself have labelled 'unsynced'.
  char hdr[128];
  snprintf(hdr, sizeof(hdr), "# resumed after dock: %lu ms gap", (unsigned long)gapMs);
  writeLine(hdr);
  snprintf(hdr, sizeof(hdr), "# clock: epoch=%llu source=%s",
           (unsigned long long)wallclock::now(), wallclock::qualityName());
  writeLine(hdr);
  flush();

  Serial.print("[dock] logging resumed -> ");
  Serial.println(s_path);
}

bool suspendedByDock() { return s_dockSuspended; }

} // namespace logger

// ---------------------------------------------------------------------------
// CAN front-end presence
// ---------------------------------------------------------------------------

namespace canfe {

namespace {
bool s_present = false;
}

bool present() { return s_present; }

bool detect(uint8_t csPin) {
  pinMode(csPin, OUTPUT);
  digitalWrite(csPin, HIGH);
  SPI.begin();
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  digitalWrite(csPin, LOW);
  SPI.transfer(0xC0); // RESET
  digitalWrite(csPin, HIGH);
  delay(10);

  digitalWrite(csPin, LOW);
  SPI.transfer(0x03); // READ
  SPI.transfer(0x0E); // CANSTAT
  const uint8_t canstat = SPI.transfer(0x00);
  digitalWrite(csPin, HIGH);

  digitalWrite(csPin, LOW);
  SPI.transfer(0x03); // READ
  SPI.transfer(0x0F); // CANCTRL
  const uint8_t canctrl = SPI.transfer(0x00);
  digitalWrite(csPin, HIGH);

  SPI.endTransaction();

  // Post-reset the MCP2515 sits in configuration mode: CANSTAT[7:5] == 0b100
  // and CANCTRL == 0x87. A floating MISO reads 0x00 or 0xFF and fails both.
  s_present = ((canstat & 0xE0) == 0x80) && (canctrl == 0x87);
  return s_present;
}

} // namespace canfe
