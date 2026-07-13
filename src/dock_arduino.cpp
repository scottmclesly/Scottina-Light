// The real dock::Platform: Serial, the Seeed FS, the raw logger, the clock.
//
// ALL of the one-open-file discipline lives here, and nowhere else. Seeed_FS.h
// is blunt about the constraint -- "Note that currently only one file can be
// open at a time" -- so this file keeps exactly one handle, remembers what it is
// for, and closes it before opening another. The protocol (src/dock_session.cpp)
// never sees a handle at all; it asks for bytes at a path.
//
// Keeping the handle open ACROSS calls matters: a GET walks a multi-MB log in
// 1 KB chunks, and re-opening and re-seeking the file for every chunk would turn
// a bounded read into thousands of FAT directory walks.

#include "dock.h"
#include "dock_platform.h"
#include "scottina_light.h"

#include <SPI.h>

#include "SD/Seeed_SD.h"
#include "Seeed_FS.h"

namespace dock {

namespace {

class ArduinoPlatform : public Platform {
public:
  // --- transport ---
  int readByte() override { return Serial.available() > 0 ? Serial.read() : -1; }
  void write(const uint8_t *data, size_t n) override {
    Serial.write(data, n);
    Serial.flush();
  }

  // --- identity ---
  uint16_t protoVersion() override { return PROTO_VERSION; }
  const char *product() override { return SL_PRODUCT; }
  const char *fwVersion() override { return SL_VERSION; }

  // --- storage ---
  bool sdMounted() override { return storage::mounted(); }

  bool exists(const char *path) override {
    if (!storage::mounted()) return false;
    release();
    return SD.exists(path);
  }

  bool remove(const char *path) override {
    if (!storage::mounted()) return false;
    release();
    return SD.remove(path);
  }

  bool rename(const char *from, const char *to) override {
    if (!storage::mounted()) return false;
    release();
    return SD.rename(from, to);
  }

  uint32_t fileSize(const char *path) override {
    if (!storage::mounted()) return 0;
    if (openFor(path, Mode::Read)) return f_.size();
    return 0;
  }

  int listDir(const char *dir, uint16_t startIndex, DirEnt *out, int maxOut,
              bool *more) override {
    *more = false;
    if (!storage::mounted()) return -1;
    release();

    // f_opendir wants "/logs", not "/logs/".
    char open[80];
    snprintf(open, sizeof(open), "%s", dir);
    size_t n = strlen(open);
    if (n > 1 && open[n - 1] == '/') open[n - 1] = '\0';

    File d = SD.open(open);
    if (!d) return -1;

    uint16_t index = 0;
    int filled = 0;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      if (e.isDirectory()) { e.close(); continue; }

      // name() returns the FULL PATH -- openNextFile builds "/logs/raw000.log"
      // and stores that. The protocol wants the LEAF. Emitting the wrong one
      // means LIST hands Prime paths where it expects filenames.
      const char *full = e.name();
      const char *slash = strrchr(full, '/');
      const char *leaf = slash ? slash + 1 : full;
      const uint32_t sz = e.size();

      // The logger's current file is never listed (§6).
      const bool isActive = logger::isOpen() && strcmp(logger::path(), full) == 0;
      e.close();
      if (isActive) continue;
      if (strlen(leaf) >= sizeof(out[0].name)) continue; // unrepresentable

      if (index++ < startIndex) continue;
      if (filled >= maxOut) { *more = true; break; }

      snprintf(out[filled].name, sizeof(out[filled].name), "%s", leaf);
      out[filled].size = sz;
      filled++;
    }
    d.close();
    return filled;
  }

  int readAt(const char *path, uint32_t offset, uint8_t *out, uint16_t n) override {
    if (!storage::mounted()) return -1;
    if (!openFor(path, Mode::Read)) return -1;
    if (offset >= f_.size()) return 0;
    if (!f_.seek(offset)) return -1;
    return f_.read(out, n);
  }

  int appendTo(const char *path, const uint8_t *data, uint16_t n) override {
    if (!storage::mounted()) return -1;
    if (!openFor(path, Mode::Append)) return -1;
    const size_t wrote = f_.write(data, n);
    f_.flush();
    return (wrote == n) ? (int)n : -1;
  }

  bool truncate(const char *path) override {
    if (!storage::mounted()) return false;
    release();
    if (SD.exists(path) && !SD.remove(path)) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    f.close();
    return true;
  }

  // --- logger ---
  void suspendLogging() override { logger::suspendForDock(); }
  void resumeLogging() override {
    release(); // hand the single handle back before the logger wants it
    logger::resumeAfterDock();
  }
  bool loggingWasActive() override { return logger::wasActiveAtDock(); }
  bool loggingSuspended() override { return logger::suspendedByDock(); }
  const char *activeLogPath() override {
    return logger::isOpen() ? logger::path() : nullptr;
  }

  // --- clock ---
  void setClock(uint64_t e, uint8_t q) override { wallclock::set(e, q); }
  uint64_t clockEpoch() override { return wallclock::now(); }
  uint8_t clockQuality() override { return wallclock::quality(); }
  bool clockSetThisBoot() override { return wallclock::setThisBoot(); }

  void diag(const char *msg) override { Serial.println(msg); }

private:
  enum class Mode : uint8_t { None, Read, Append };

  File f_;
  Mode mode_ = Mode::None;
  char path_[80] = {0};

  void release() {
    if (mode_ != Mode::None) {
      f_.close();
      mode_ = Mode::None;
      path_[0] = '\0';
    }
  }

  // The whole one-handle rule, in one place. Reuse the open file when the caller
  // wants the same path in the same mode -- that is what keeps a chunked GET
  // from re-walking the FAT for every kilobyte.
  bool openFor(const char *path, Mode m) {
    if (mode_ == m && strcmp(path_, path) == 0) return true;
    release();
    f_ = SD.open(path, (m == Mode::Read) ? FILE_READ : FILE_APPEND);
    if (!f_) return false;
    mode_ = m;
    snprintf(path_, sizeof(path_), "%s", path);
    return true;
  }
};

ArduinoPlatform s_arduino;

} // namespace

void begin() {
  setPlatform(&s_arduino);
  reset();
}

} // namespace dock
