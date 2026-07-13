// The seam that makes the dock handler provable without a board.
//
// Everything the protocol handler needs from the outside world -- the serial
// port, the SD card, the logger, the clock -- arrives through this interface.
// src/dock_session.cpp then contains the ENTIRE protocol (framing, paging,
// staging, hashing, the reject pass, the watchdog) and touches no hardware, so
// all 47 conformance vectors replay against it on a laptop.
//
// Two implementations:
//   src/dock_arduino.cpp        -- the real one: Serial + Seeed FS + logger
//   test/test_dock/fake_platform.h -- in-memory, drives the vector replay
//
// Note the file API is PATH-based, not handle-based. That is deliberate: the FS
// layer permits exactly one open file at a time ("Note that currently only one
// file can be open at a time" -- Seeed_FS.h), and that constraint is the real
// implementation's problem to solve, not the protocol's. Hiding it here keeps
// the handle discipline in one place instead of smeared through every command.

#ifndef SL_DOCK_PLATFORM_H
#define SL_DOCK_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

namespace dock {

struct DirEnt {
  char name[64]; // LEAF name only -- no directory part
  uint32_t size;
};

class Platform {
public:
  virtual ~Platform() {}

  // --- transport ---
  virtual int readByte() = 0; // -1 when nothing is waiting
  virtual void write(const uint8_t *data, size_t n) = 0;

  // --- identity (HELLO) ---
  // protoVersion() is a method rather than a constant so the version-mismatch
  // backstop (§8) can actually be exercised by a test.
  virtual uint16_t protoVersion() = 0;
  virtual const char *product() = 0;
  virtual const char *fwVersion() = 0;

  // --- storage ---
  virtual bool sdMounted() = 0;
  virtual bool exists(const char *path) = 0;
  virtual bool remove(const char *path) = 0;
  virtual bool rename(const char *from, const char *to) = 0;
  virtual uint32_t fileSize(const char *path) = 0; // 0 if absent

  // Fills up to maxOut entries starting at startIndex. Returns the number
  // filled, or -1 if the directory does not exist. Sets *more when entries
  // remain beyond the ones returned.
  virtual int listDir(const char *dir, uint16_t startIndex, DirEnt *out,
                      int maxOut, bool *more) = 0;

  virtual int readAt(const char *path, uint32_t offset, uint8_t *out, uint16_t n) = 0;
  // Appends, creating the file if absent. Returns bytes written, or -1.
  virtual int appendTo(const char *path, const uint8_t *data, uint16_t n) = 0;
  virtual bool truncate(const char *path) = 0; // create empty, or empty an existing one

  // --- logger (§6) ---
  virtual void suspendLogging() = 0;
  virtual void resumeLogging() = 0;
  virtual bool loggingWasActive() = 0;  // was a capture running when we docked?
  virtual bool loggingSuspended() = 0;  // is it suspended by the dock right now?
  virtual const char *activeLogPath() = 0; // nullptr when no file is held open

  // --- clock (§4 SET_CLOCK) ---
  virtual void setClock(uint64_t epoch, uint8_t quality) = 0;
  virtual uint64_t clockEpoch() = 0;
  virtual uint8_t clockQuality() = 0;
  virtual bool clockSetThisBoot() = 0;

  // Light's own record. Every rejection is logged on the device that was asked,
  // not merely reported to the asker.
  virtual void diag(const char *msg) = 0;
};

void setPlatform(Platform *p);
Platform *platform();

// Tears down any in-flight session state. Called at boot, and between vectors.
void reset();

} // namespace dock

#endif // SL_DOCK_PLATFORM_H
