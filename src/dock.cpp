// Light Dock -- protocol handler. DOCK-PROTOCOL.md is the contract; this file
// implements the Light (responder) half of it.
//
// Prime is the only initiator; Light only ever replies, strictly sequentially.
//
// THE CONSTRAINT THAT SHAPES EVERYTHING HERE: the FS layer permits exactly one
// open file at a time. Seeed_FS.h says so outright -- "Note that currently only
// one file can be open at a time." Hence:
//
//   * a dock session SUSPENDS the raw logger (§6) -- it cannot hold the handle;
//   * exactly one file handle exists below (s_file) with an explicit owner, and
//     switching owners closes it first;
//   * GET hashes as it streams, so DELETE's verify does not need a second read.

#include "dock.h"

#include "scottina_light.h"
#include "sha256.h"

#include <SPI.h>

#include "SD/Seeed_SD.h"
#include "Seeed_FS.h"

namespace dock {

namespace {

// --- session state ----------------------------------------------------------

bool s_active = false;
uint32_t s_lastFrameMs = 0;

// RX accumulator. One MAX_PAYLOAD frame plus overhead, plus slack so a frame
// that arrives split across reads still lands contiguously.
constexpr size_t RX_CAP = MAX_PAYLOAD + FRAME_OVERHEAD + 64;
uint8_t s_rx[RX_CAP];
size_t s_rxN = 0;

uint8_t s_tx[MAX_PAYLOAD + FRAME_OVERHEAD];

// --- the single file handle -------------------------------------------------

enum class Owner : uint8_t { None, Put, Get };
File s_file;
Owner s_owner = Owner::None;
char s_openPath[80];

void releaseFile() {
  if (s_owner != Owner::None) {
    s_file.close();
    s_owner = Owner::None;
    s_openPath[0] = '\0';
  }
}

// --- streaming digest (GET) -------------------------------------------------
// While a file streams from offset 0 to eof we hash exactly the bytes we put on
// the wire, then cache the result. DELETE then verifies for free. Re-reading an
// 8 MB log to hash it costs 16-27 s over the 8 MHz SPI-SD bus -- see §5.

sha256_ctx s_getCtx;
char s_getPath[80];
uint32_t s_getNext = 0; // next contiguous offset we expect
bool s_getHashing = false;

struct DigestCache {
  char path[80];
  uint32_t size;
  uint8_t digest[32];
  bool valid;
} s_cache = {{0}, 0, {0}, false};

// Hard cap on entries per LIST page, independent of the byte budget. Short
// filenames could otherwise pack ~67 entries into 1024 bytes; this bounds the
// static buffer instead of letting the wire decide how much RAM we spend.
// Prime simply paginates once more -- `more` already tells it to.
constexpr uint16_t MAX_ENTRIES = 48;

// --- PUT staging ------------------------------------------------------------

char s_putPath[80];     // the FINAL path
char s_putStaging[88];  // the .partial we actually write
uint32_t s_putNext = 0;

void stagingPathFor(const char *finalPath, char *out, size_t cap) {
  snprintf(out, cap, "%s.partial", finalPath);
}

// --- replies ----------------------------------------------------------------

void sendFrame(uint8_t type, uint8_t seq, const uint8_t *pl, uint16_t n) {
  const size_t total = encode(type, seq, pl, n, s_tx, sizeof(s_tx));
  if (!total) return; // refuses to emit an oversized frame rather than truncate
  Serial.write(s_tx, total);
  Serial.flush();
}

void sendError(uint8_t seq, uint16_t code, const char *msg) {
  uint8_t pl[80];
  Writer w(pl, sizeof(pl));
  w.u16v(code);
  w.str(msg);
  if (!w.ok()) return;
  sendFrame(T_ERROR, seq, pl, (uint16_t)w.size());

  // Every rejection is logged on Light, not just reported to Prime. If Prime is
  // asking for things it should not, the device that was asked keeps the record.
  Serial.print("[dock] error 0x");
  Serial.print(code, HEX);
  Serial.print(" -- ");
  Serial.println(msg);
}

// Hashes a whole file by reading it. The expensive path -- only reached when the
// digest was not already cached by the GET that streamed it, which in practice
// means a redock that resumes an interrupted pull. That is precisely why DELETE
// is given 60 s in §5 rather than 30.
bool hashFile(const char *path, uint8_t out[32], uint32_t *sizeOut) {
  releaseFile();
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  sha256_ctx c;
  sha256_init(&c);
  uint8_t buf[512];
  uint32_t total = 0;
  while (true) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    sha256_update(&c, buf, (size_t)n);
    total += (uint32_t)n;
  }
  f.close();
  sha256_final(&c, out);
  if (sizeOut) *sizeOut = total;
  return true;
}

// --- command handlers -------------------------------------------------------

void doHello(uint8_t seq) {
  uint8_t pl[96];
  Writer w(pl, sizeof(pl));
  w.u16v(PROTO_VERSION);
  w.str(SL_PRODUCT);
  w.str(SL_VERSION);
  w.u8v(storage::mounted() ? 1 : 0);
  w.u64v(wallclock::now());
  w.u8v(wallclock::quality());
  w.u8v(wallclock::setThisBoot() ? 1 : 0);
  w.u16v(MAX_PAYLOAD);

  // bit0: logging was active at dock. bit1: it is suspended right now.
  // Prime prints this. The cost of docking is stated, never silent.
  uint8_t flags = 0;
  if (logger::suspendedByDock()) flags |= 0x02;
  if (logger::suspendedByDock() || logger::isOpen()) flags |= 0x01;
  w.u8v(flags);

  if (!w.ok()) { sendError(seq, E_IO, "hello overflow"); return; }
  sendFrame(T_HELLO, seq, pl, (uint16_t)w.size());
}

void doSetClock(uint8_t seq, Reader &r) {
  const uint64_t epoch = r.u64v();
  const uint8_t quality = r.u8v();
  if (!r.ok()) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  // §4: Prime MUST NOT send quality=unsynced with a nonzero epoch. Enforced
  // here rather than trusted -- a bad clock is labelled, never laundered into
  // the black box as truth.
  if (quality == Q_UNSYNCED && epoch != 0) {
    sendError(seq, E_BAD_FRAME, "bad frame");
    return;
  }
  if (quality > Q_NTP) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  wallclock::set(epoch, quality);
  Serial.print("[dock] clock set: epoch=");
  Serial.print((unsigned long)epoch);
  Serial.print(" source=");
  Serial.println(wallclock::qualityName());

  uint8_t pl[9];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  w.u64v(epoch);
  sendFrame(T_SET_CLOCK, seq, pl, (uint16_t)w.size());
}

void doList(uint8_t seq, Reader &r) {
  char dir[80];
  if (!r.str(dir, sizeof(dir))) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }
  const uint8_t wantHashes = r.u8v();
  const uint16_t startIndex = r.u16v();
  if (!r.ok()) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(dir, Op::List);
  if (why) { sendError(seq, E_PATH_REJECTED, why); return; }
  if (!storage::mounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  // Open WITHOUT the trailing slash -- the protocol names dirs as "/logs/", but
  // f_opendir wants "/logs".
  char dirOpen[80];
  snprintf(dirOpen, sizeof(dirOpen), "%s", dir);
  size_t dlen = strlen(dirOpen);
  if (dlen > 1 && dirOpen[dlen - 1] == '/') dirOpen[dlen - 1] = '\0';

  // TWO PASSES, and the split is forced by the FS, not chosen for tidiness:
  //   1. enumerate the page, then CLOSE the directory;
  //   2. only then hash, because hashing opens a file and openNextFile() opens
  //      one too (it calls f_open on every entry), and only one may be open.
  // Doing both at once means holding two handles, which this FS layer does not
  // have.
  releaseFile();

  File d = SD.open(dirOpen);
  if (!d) { sendError(seq, E_NOT_FOUND, "not found"); return; }

  struct Ent {
    char name[64]; // leaf only
    uint32_t size;
  };
  static Ent ents[MAX_ENTRIES];

  uint16_t index = 0, count = 0;
  uint8_t more = 0;
  size_t used = 4; // u16 start_index + u16 count
  const size_t budget = MAX_PAYLOAD - 1; // reserve the trailing `more` byte

  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }

    // name() returns the FULL PATH ("/logs/raw000.log") -- openNextFile builds
    // it that way. The protocol wants the LEAF. Getting this wrong emits paths
    // where the vectors expect filenames.
    const char *full = f.name();
    const char *slash = strrchr(full, '/');
    const char *leaf = slash ? slash + 1 : full;
    const uint32_t sz = f.size();

    // The logger's CURRENT file is never served (§6). The dock has already
    // suspended logging by the time we get here, so this is belt and braces --
    // but it is the black box, so it wears both.
    const bool isActiveLog = logger::isOpen() && strcmp(logger::path(), full) == 0;
    f.close();
    if (isActiveLog) continue;

    if (strlen(leaf) >= sizeof(ents[0].name)) continue; // unrepresentable; skip
    if (index++ < startIndex) continue;

    const size_t need = 1 + strlen(leaf) + 4 + 8 + 1 + (wantHashes ? 32 : 0);
    if (count >= MAX_ENTRIES || used + need > budget) {
      // Out of room. Stop cleanly and tell Prime to come back for the rest. An
      // oversized reply is not a soft failure -- §2 makes Prime DISCARD it, so
      // the sync would break exactly when the card holds the most to rescue.
      more = 1;
      break;
    }
    snprintf(ents[count].name, sizeof(ents[count].name), "%s", leaf);
    ents[count].size = sz;
    used += need;
    count++;
  }
  d.close(); // the directory handle is released BEFORE any file is hashed

  uint8_t pl[MAX_PAYLOAD];
  Writer w(pl, sizeof(pl) - 1);
  w.u16v(startIndex);
  w.u16v(count);

  for (uint16_t i = 0; i < count; ++i) {
    w.str(ents[i].name);
    w.u32v(ents[i].size);
    w.u64v(0); // mtime: advisory only, and meaningless on a clock-less card
    if (!wantHashes) {
      w.u8v(0);
      continue;
    }
    char full[80];
    snprintf(full, sizeof(full), "%s%s", dir, ents[i].name);
    uint8_t dg[32];
    if (hashFile(full, dg, nullptr)) {
      w.u8v(1);
      w.bytes(dg, 32);
    } else {
      w.u8v(0); // Light MAY decline to hash; Prime falls back to name+size
    }
  }

  if (!w.ok()) { sendError(seq, E_IO, "list overflow"); return; }

  size_t n = w.size();
  pl[n++] = more;
  sendFrame(T_LIST, seq, pl, (uint16_t)n);
}

void doPut(uint8_t seq, Reader &r) {
  char path[80];
  if (!r.str(path, sizeof(path))) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }
  const uint32_t offset = r.u32v();
  const uint16_t chunkLen = r.u16v();
  if (!r.ok() || chunkLen > MAX_PAYLOAD) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Write);
  if (why) { sendError(seq, E_PATH_REJECTED, why); return; }
  if (!storage::mounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  uint8_t chunk[MAX_PAYLOAD];
  if (chunkLen && !r.bytes(chunk, chunkLen)) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  // Writes are STAGED, never live. The destination does not exist -- and is not
  // modified -- until COMMIT verifies the hash and renames. No commit, no file.
  char staging[88];
  stagingPathFor(path, staging, sizeof(staging));

  if (offset == 0) {
    releaseFile();
    if (SD.exists(staging)) SD.remove(staging);
    s_file = SD.open(staging, FILE_WRITE);
    if (!s_file) { sendError(seq, E_IO, "cannot stage"); return; }
    s_owner = Owner::Put;
    snprintf(s_putPath, sizeof(s_putPath), "%s", path);
    snprintf(s_putStaging, sizeof(s_putStaging), "%s", staging);
    s_putNext = 0;
  } else {
    // Chunks must be contiguous and ascending. A gap would leave a hole we
    // would then cheerfully hash and commit.
    if (s_owner != Owner::Put || strcmp(s_putPath, path) != 0 || offset != s_putNext) {
      sendError(seq, E_IO, "out of order chunk");
      return;
    }
  }

  if (chunkLen) {
    const size_t wrote = s_file.write(chunk, chunkLen);
    if (wrote != chunkLen) { sendError(seq, E_IO, "short write"); return; }
    s_putNext = offset + chunkLen;
    s_file.flush();
  }

  uint8_t pl[5];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  w.u32v(s_putNext);
  sendFrame(T_PUT, seq, pl, (uint16_t)w.size());
}

void doCommit(uint8_t seq, Reader &r) {
  char path[80];
  if (!r.str(path, sizeof(path))) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }
  uint8_t want[32];
  if (!r.bytes(want, 32)) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Write);
  if (why) { sendError(seq, E_PATH_REJECTED, why); return; }
  if (!storage::mounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  char staging[88];
  stagingPathFor(path, staging, sizeof(staging));

  releaseFile(); // close the PUT handle so we can read the staged file back
  if (!SD.exists(staging)) { sendError(seq, E_NOT_FOUND, "not found"); return; }

  uint8_t got[32];
  if (!hashFile(staging, got, nullptr)) { sendError(seq, E_IO, "cannot read staging"); return; }

  if (memcmp(got, want, 32) != 0) {
    // Verify BEFORE rename, never after. A staged file that does not hash is
    // not a file, it is noise -- unlink it rather than leave it to be retried.
    SD.remove(staging);
    sendError(seq, E_HASH_MISMATCH, "hash mismatch");
    return;
  }

  if (SD.exists(path)) SD.remove(path); // Prime always wins: overwrite, never merge
  if (!SD.rename(staging, path)) {
    sendError(seq, E_IO, "rename failed");
    return;
  }

  Serial.print("[dock] committed ");
  Serial.println(path);

  uint8_t pl[1];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  sendFrame(T_COMMIT, seq, pl, (uint16_t)w.size());
}

void doGet(uint8_t seq, Reader &r) {
  char path[80];
  if (!r.str(path, sizeof(path))) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }
  const uint32_t offset = r.u32v();
  uint16_t maxLen = r.u16v();
  if (!r.ok()) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Read);
  if (why) { sendError(seq, E_PATH_REJECTED, why); return; }
  if (!storage::mounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  // The response carries u32 offset + u16 len + data + u8 eof, so the data we
  // can actually return is MAX_PAYLOAD minus that 7-byte envelope.
  constexpr uint16_t ENVELOPE = 4 + 2 + 1;
  if (maxLen > MAX_PAYLOAD - ENVELOPE) maxLen = MAX_PAYLOAD - ENVELOPE;

  if (s_owner != Owner::Get || strcmp(s_openPath, path) != 0) {
    releaseFile();
    s_file = SD.open(path, FILE_READ);
    if (!s_file) { sendError(seq, E_NOT_FOUND, "not found"); return; }
    s_owner = Owner::Get;
    snprintf(s_openPath, sizeof(s_openPath), "%s", path);
  }

  const uint32_t size = s_file.size();

  // Start of file: begin a streaming hash of exactly the bytes we send.
  if (offset == 0) {
    sha256_init(&s_getCtx);
    snprintf(s_getPath, sizeof(s_getPath), "%s", path);
    s_getNext = 0;
    s_getHashing = true;
  } else if (!s_getHashing || strcmp(s_getPath, path) != 0 || offset != s_getNext) {
    // Random access -- we cannot fold this into a running digest. Abandon the
    // streaming hash; DELETE will fall back to re-reading the file.
    s_getHashing = false;
  }

  uint16_t n = 0;
  uint8_t data[MAX_PAYLOAD];

  if (offset < size) {
    if (!s_file.seek(offset)) { sendError(seq, E_IO, "seek failed"); return; }
    const uint32_t remain = size - offset;
    const uint16_t want = (uint16_t)((remain < maxLen) ? remain : maxLen);
    const int got = s_file.read(data, want);
    if (got < 0) { sendError(seq, E_IO, "read failed"); return; }
    n = (uint16_t)got;
    if (s_getHashing) {
      sha256_update(&s_getCtx, data, n);
      s_getNext = offset + n;
    }
  }
  // offset at or past EOF is benign: len=0, eof=1, not an error. Keeps a
  // re-pull after an interrupted dock free of edge cases.

  const uint8_t eof = (offset + n >= size) ? 1 : 0;

  if (eof && s_getHashing && s_getNext == size) {
    // We streamed the whole file and hashed exactly what went on the wire --
    // which is precisely the claim DELETE will ask us to check. Cache it.
    snprintf(s_cache.path, sizeof(s_cache.path), "%s", path);
    s_cache.size = size;
    sha256_ctx c = s_getCtx; // final() consumes the context
    sha256_final(&c, s_cache.digest);
    s_cache.valid = true;
  }

  uint8_t pl[MAX_PAYLOAD];
  Writer w(pl, sizeof(pl));
  w.u32v(offset);
  w.u16v(n);
  w.bytes(data, n);
  w.u8v(eof);
  if (!w.ok()) { sendError(seq, E_IO, "get overflow"); return; }
  sendFrame(T_GET, seq, pl, (uint16_t)w.size());
}

void doDelete(uint8_t seq, Reader &r) {
  char path[80];
  if (!r.str(path, sizeof(path))) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }
  uint8_t want[32];
  if (!r.bytes(want, 32)) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Delete);
  if (why) { sendError(seq, E_PATH_REJECTED, why); return; }
  if (!storage::mounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  releaseFile();
  if (!SD.exists(path)) { sendError(seq, E_NOT_FOUND, "not found"); return; }

  // THE SCARY STEP, and deliberately the most defended. A pull that verifies
  // nothing is a copy; a delete that verifies nothing is data loss on a black
  // box. We unlink on a hash match and on nothing else.
  uint8_t got[32];
  if (s_cache.valid && strcmp(s_cache.path, path) == 0) {
    memcpy(got, s_cache.digest, 32); // hashed as we streamed it -- free
  } else if (!hashFile(path, got, nullptr)) {
    sendError(seq, E_IO, "cannot read");
    return;
  }

  if (memcmp(got, want, 32) != 0) {
    sendError(seq, E_HASH_MISMATCH, "hash mismatch");
    return;
  }
  if (!SD.remove(path)) { sendError(seq, E_IO, "unlink failed"); return; }

  if (strcmp(s_cache.path, path) == 0) s_cache.valid = false;
  Serial.print("[dock] deleted (verified) ");
  Serial.println(path);

  uint8_t pl[1];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  sendFrame(T_DELETE, seq, pl, (uint16_t)w.size());
}

// Ends the session and hands the port -- and the file handle -- back.
void endSession(const char *reason) {
  if (!s_active) return;
  releaseFile();
  s_getHashing = false;
  s_cache.valid = false;
  s_active = false;
  s_rxN = 0;

  Serial.print("[dock] session ended (");
  Serial.print(reason);
  Serial.println(") -- standalone");

  logger::resumeAfterDock(); // a yanked cable must NEVER leave logging off
}

void dispatch(const Frame &f) {
  Reader r(f.payload, f.len);

  // §8: on a version mismatch, everything outside the every-version set
  // (HELLO, SET_CLOCK, BYE) is refused. A well-behaved Prime degrades on its
  // own and never gets here; this is the backstop for one that does not.
  const bool everyVersion =
      (f.type == T_HELLO || f.type == T_SET_CLOCK || f.type == T_BYE);
  if (!everyVersion && PROTO_VERSION != 1) {
    sendError(f.seq, E_UNSUPPORTED_VER, "unsupported version");
    return;
  }

  switch (f.type) {
  case T_HELLO: doHello(f.seq); break;
  case T_SET_CLOCK: doSetClock(f.seq, r); break;
  case T_LIST: doList(f.seq, r); break;
  case T_PUT: doPut(f.seq, r); break;
  case T_COMMIT: doCommit(f.seq, r); break;
  case T_GET: doGet(f.seq, r); break;
  case T_DELETE: doDelete(f.seq, r); break;
  case T_BYE: {
    uint8_t pl[1] = {1};
    sendFrame(T_BYE, f.seq, pl, 1);
    endSession("bye");
    break;
  }
  default:
    // Positive allow-list: anything not above is refused by construction. This
    // is the line that keeps the dock to provisioning + retrieval.
    sendError(f.seq, E_UNKNOWN_TYPE, "unknown type");
    break;
  }
}

void beginSession() {
  if (s_active) return;
  s_active = true;
  Serial.println("[dock] framed traffic -- dock owns the port");
  // Take the file handle away from the logger before serving anything. One open
  // file at a time is not a preference, it is the FS layer's hard limit.
  logger::suspendForDock();
}

} // namespace

// --- public -----------------------------------------------------------------

bool active() { return s_active; }
bool loggingSuspended() { return logger::suspendedByDock(); }

void tick(uint32_t nowMs) {
  // Drain the port. MUST run before input::poll(), which swallows any available
  // byte (core_ui.cpp `default: break`) and would eat the frame stream whole.
  while (Serial.available() > 0 && s_rxN < RX_CAP) {
    const int c = Serial.read();
    if (c < 0) break;
    s_rx[s_rxN++] = (uint8_t)c;
  }

  // Buffer full with no parseable frame in it: it is garbage, not a frame.
  // Drop the oldest half rather than wedge. Bounded reads, always.
  if (s_rxN >= RX_CAP) {
    memmove(s_rx, s_rx + RX_CAP / 2, RX_CAP - RX_CAP / 2);
    s_rxN = RX_CAP - RX_CAP / 2;
  }

  size_t pos = 0;
  while (pos < s_rxN) {
    Frame f;
    size_t used = 0;
    const Parse p = parse(s_rx + pos, s_rxN - pos, f, used);

    if (p == Parse::NeedMore) break;

    if (p == Parse::Ok) {
      beginSession();
      s_lastFrameMs = nowMs;
      dispatch(f);
      pos += used;
      if (!s_active) { pos = s_rxN; break; } // BYE tore the session down
      continue;
    }

    if (p == Parse::BadCrc) {
      // Complete, in-range, corrupt. Reply -- echoing a SEQ we cannot vouch for,
      // which Prime treats as advisory -- then resync one byte on.
      if (s_active) {
        s_lastFrameMs = nowMs;
        sendError(f.seq, E_BAD_CRC, "bad crc");
      }
      pos += used;
      continue;
    }

    pos += used; // Parse::Discard -- silence, by design (§2)
  }

  if (pos > 0) {
    memmove(s_rx, s_rx + pos, s_rxN - pos);
    s_rxN -= pos;
  }

  // The 10 s watchdog. The ONE place Light acts without being asked, and it
  // exists to protect the black box: a yanked cable mid-session must not leave
  // logging switched off forever.
  if (s_active && (nowMs - s_lastFrameMs) > WATCHDOG_MS) {
    endSession("watchdog -- no frame in 10s");
  }
}

} // namespace dock
