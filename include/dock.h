// ============================================================================
//  Light Dock -- USB CDC dock protocol.  See DOCK-PROTOCOL.md (the contract).
// ============================================================================
//
//  Provisioning + retrieval ONLY. This module can set the clock, write tables
//  and Tier-3 config, and read CLOSED log files. It deliberately cannot trigger
//  transmission, start or stop a capture remotely, or execute anything. Adding
//  such a command is a scope violation -- see include/scope.h.
//
//  Everything above the ---- ARDUINO ---- line is pure: <stdint.h>/<string.h>
//  only, no Arduino, no SD. That is what lets the frame codec and the reject
//  pass run natively against all 47 conformance vectors (`pio test -e native`)
//  with no hardware in the loop.
// ============================================================================

#ifndef SL_DOCK_H
#define SL_DOCK_H

#include <stddef.h>
#include <stdint.h>

namespace dock {

// --- protocol constants (DOCK-PROTOCOL.md §2, §3) ---------------------------

constexpr uint8_t SOF = 0xA5;
constexpr uint16_t PROTO_VERSION = 1;
constexpr uint16_t MAX_PAYLOAD = 1024;
constexpr uint16_t FRAME_OVERHEAD = 7; // SOF + TYPE + SEQ + LEN(2) + CRC(2)
constexpr uint32_t WATCHDOG_MS = 10000;

enum Type : uint8_t {
  T_HELLO = 0x01,
  T_SET_CLOCK = 0x02,
  T_LIST = 0x03,
  T_PUT = 0x04,
  T_COMMIT = 0x05,
  T_GET = 0x06,
  T_DELETE = 0x07,
  T_BYE = 0x08,
  T_ERROR = 0xEF,
};

enum Err : uint16_t {
  E_BAD_CRC = 0x0001,
  E_BAD_FRAME = 0x0002,
  E_UNKNOWN_TYPE = 0x0003,
  E_NO_SD = 0x0004,
  E_PATH_REJECTED = 0x0005,
  E_NOT_FOUND = 0x0006,
  E_IO = 0x0007,
  E_HASH_MISMATCH = 0x0008,
  E_BUSY = 0x0009,
  E_UNSUPPORTED_VER = 0x000A,
  E_TOO_LARGE = 0x000B,
};

enum ClockQuality : uint8_t { Q_UNSYNCED = 0, Q_RTC = 1, Q_NTP = 2 };

// --- frame codec (pure) -----------------------------------------------------

struct Frame {
  uint8_t type;
  uint8_t seq;
  uint16_t len;
  const uint8_t *payload; // points into the caller's buffer; not owned
};

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.
// Check value for "123456789" is 0x29B1 (asserted in the test suite).
uint16_t crc16(const uint8_t *data, size_t n);

enum class Parse : uint8_t {
  Ok,       // a valid frame is at the front of buf; `consumed` bytes eaten
  NeedMore, // a plausible frame is in progress but incomplete; consume nothing
  BadCrc,   // a complete, in-range frame failed CRC -- reply ERR_BAD_CRC, then
            // resync. `frame.type`/`frame.seq` are the RECEIVED (corrupt) bytes
            // and are ADVISORY ONLY: they come from a frame we just proved we
            // cannot trust. Echoing them is a courtesy to Prime's logs, not an
            // identification. `consumed` == 1 (discard one byte, rescan).
  Discard,  // garbage, or a header we cannot trust enough to answer (LEN over
            // MAX_PAYLOAD). Emit NOTHING -- an unvalidated header has no SEQ we
            // may echo. `consumed` bytes are dropped.
};

// Attempts to parse one frame at the front of `buf`. Never reads past `len`,
// never allocates, never trusts LEN before bounding it (§2 resync rule).
Parse parse(const uint8_t *buf, size_t len, Frame &out, size_t &consumed);

// Encodes a frame into `out` (must hold FRAME_OVERHEAD + payloadLen bytes).
// Returns the total frame length, or 0 if payloadLen exceeds MAX_PAYLOAD.
size_t encode(uint8_t type, uint8_t seq, const uint8_t *payload,
              uint16_t payloadLen, uint8_t *out, size_t outCap);

// --- payload writer (pure) --------------------------------------------------
// Little-endian everything; strings are u8 length + UTF-8, no NUL (§2).
// Every put() is bounds-checked; overflow latches `ok=false` rather than
// scribbling past the buffer, and the caller checks once at the end.

class Writer {
public:
  Writer(uint8_t *buf, size_t cap) : b_(buf), cap_(cap) {}
  void u8v(uint8_t v);
  void u16v(uint16_t v);
  void u32v(uint32_t v);
  void u64v(uint64_t v);
  void bytes(const uint8_t *p, size_t n);
  void str(const char *s);
  size_t size() const { return n_; }
  bool ok() const { return ok_; }
  // True if `extra` more bytes would still fit. Used by LIST to stop filling a
  // page before it overflows, rather than after.
  bool fits(size_t extra) const { return ok_ && n_ + extra <= cap_; }

private:
  uint8_t *b_;
  size_t cap_;
  size_t n_ = 0;
  bool ok_ = true;
};

// --- payload reader (pure) --------------------------------------------------
// Symmetric to Writer: a short read latches `ok=false` instead of running off
// the end. A malformed payload therefore becomes ERR_BAD_FRAME, not a crash.

class Reader {
public:
  Reader(const uint8_t *buf, size_t len) : b_(buf), len_(len) {}
  uint8_t u8v();
  uint16_t u16v();
  uint32_t u32v();
  uint64_t u64v();
  bool bytes(uint8_t *out, size_t n);
  // Copies a length-prefixed string into `out` (NUL-terminated). Fails if the
  // string does not fit -- paths are bounded, so an over-long one is malformed.
  //
  // `declaredLen` receives the length the WIRE claimed, which is not necessarily
  // strlen(out): the format is length-prefixed, so it can carry an embedded NUL
  // that C string handling cannot. Callers dealing in paths MUST compare the two
  // (see readPath in dock_session.cpp) -- a silently truncated path would have
  // Light act on a DIFFERENT FILE than the one it was asked about.
  bool str(char *out, size_t outCap, size_t *declaredLen = nullptr);
  bool ok() const { return ok_; }
  bool done() const { return i_ == len_; }

private:
  const uint8_t *b_;
  size_t len_;
  size_t i_ = 0;
  bool ok_ = true;
};

// --- path policy (pure) -- DOCK-PROTOCOL.md §7 ------------------------------
//
// Two independent layers, deliberately. The allow-list alone would be
// sufficient; the reject pass exists because "sufficient" is not the standard
// for the one component that can delete a black box.

enum class Op : uint8_t { Read, Write, Delete, List };

// True if `path` may be used for `op`. Runs the reject pass FIRST (traversal,
// backslash, non-rooted, control bytes), then the positive allow-list.
bool pathAllowed(const char *path, Op op);

// Why a path was rejected -- for Light's own log line. Returns nullptr if the
// path is allowed.
const char *rejectReason(const char *path, Op op);

// ---- ARDUINO ---------------------------------------------------------------
//
// The protocol itself is NOT below this line -- it lives in dock_session.cpp and
// reaches the world only through dock::Platform (include/dock_platform.h), which
// is what lets all 47 conformance vectors replay on a laptop. Only the wiring is
// here.

// Installs the real (Serial + SD + logger) platform. Call once, in setup().
void begin();

// Drains the CDC port and services the dock. MUST be called BEFORE
// input::poll() in the main loop: the SL_TEST_HOOK reader consumes any
// available serial byte and SWALLOWS unmatched ones (core_ui.cpp,
// `default: break`), so it would silently eat the frame stream. Choosing a
// high-bit SOF is necessary and nowhere near sufficient.
void tick(uint32_t nowMs);

// True once a valid frame has arrived and the dock owns the port. While this
// holds, the test hook must not touch Serial at all.
bool active();

// One byte that tick() drained but which cannot belong to a frame, or -1 when
// there are none. Because tick() empties the port, this is the ONLY way anyone
// else can still see those bytes -- reading Serial directly races the drain and
// loses. The SL_TEST_HOOK reader consumes this instead.
int takeStray();

// True while the dock has suspended the raw logger (§6). The FS layer permits
// exactly one open file at a time, so serving a GET and logging concurrently is
// not merely undesirable -- it is impossible.
bool loggingSuspended();

} // namespace dock

#endif // SL_DOCK_H
