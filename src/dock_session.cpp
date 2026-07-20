// Light Dock -- the protocol, entire. DOCK-PROTOCOL.md is the contract.
//
// This file is PURE: it reaches the world only through dock::Platform, so all 47
// conformance vectors replay against it on a laptop with no board attached
// (`pio test -e native`). The Arduino half is src/dock_arduino.cpp.
//
// Prime is the only initiator. Light only ever replies, strictly sequentially.

#include "dock.h"
#include "dock_platform.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

namespace dock {

namespace {

Platform *s_plat = nullptr;
inline Platform &P() { return *s_plat; }

bool s_active = false;
uint32_t s_lastFrameMs = 0;

constexpr size_t RX_CAP = MAX_PAYLOAD + FRAME_OVERHEAD + 64;
uint8_t s_rx[RX_CAP];

// Bytes drained off the port that cannot begin a frame, held for whoever else
// wants them (the SL_TEST_HOOK reader). Bounded and lossy on purpose: a mashed
// thumb switch must never be able to grow this.
uint8_t s_stray[32];
uint8_t s_strayN = 0;
size_t s_rxN = 0;
uint8_t s_tx[MAX_PAYLOAD + FRAME_OVERHEAD];

// Hard cap on entries per LIST page, independent of the byte budget. Short
// filenames could otherwise pack ~67 entries into 1024 bytes; this bounds the
// buffer rather than letting the wire decide how much RAM we spend. Prime just
// paginates once more -- `more` already tells it to.
constexpr int MAX_ENTRIES = 48;

// --- streaming digest (GET) -------------------------------------------------
// A file streamed from offset 0 to eof is hashed as it goes, over exactly the
// bytes we put on the wire -- which is precisely the claim DELETE will ask us to
// check. Re-reading an 8 MB log to hash it costs 16-27 s over the SPI-SD bus.

sha256_ctx s_getCtx;
char s_getPath[80];
uint32_t s_getNext = 0;
bool s_getHashing = false;

struct DigestCache {
  char path[80];
  uint8_t digest[32];
  bool valid;
} s_cache;

void stagingPathFor(const char *finalPath, char *out, size_t cap) {
  snprintf(out, cap, "%s.partial", finalPath);
}

// --- replies ----------------------------------------------------------------

void sendFrame(uint8_t type, uint8_t seq, const uint8_t *pl, uint16_t n) {
  const size_t total = encode(type, seq, pl, n, s_tx, sizeof(s_tx));
  if (!total) return; // refuse to emit an oversized frame rather than truncate
  P().write(s_tx, total);
}

void sendError(uint8_t seq, uint16_t code, const char *msg) {
  uint8_t pl[80];
  Writer w(pl, sizeof(pl));
  w.u16v(code);
  w.str(msg);
  if (!w.ok()) return;
  sendFrame(T_ERROR, seq, pl, (uint16_t)w.size());

  // Every rejection is recorded on the device that was ASKED, not only reported
  // to the asker. If Prime is requesting things it should not, Light keeps its
  // own account of it.
  char line[128];
  snprintf(line, sizeof(line), "[dock] error 0x%04X -- %s", code, msg);
  P().diag(line);
}

// Reads a path and proves it is safe to treat as a C string.
//
// The wire format is LENGTH-PREFIXED, so it can carry an embedded NUL. C string
// handling cannot. Copy `/logs/raw\0.log` into a char[] and strlen() reports
// `/logs/raw` -- a DIFFERENT, possibly existing file, which sails through the
// reject pass because there is nothing left in it to reject. For DELETE that is
// unlinking the wrong log off a black box.
//
// So the declared length and the C length must agree. They disagree only when a
// NUL is embedded, and that is a path rejection (§7), not a framing error: the
// frame parsed perfectly, the path is what is unusable.
//
// Returns false having ALREADY replied.
bool readPath(uint8_t seq, Reader &r, char *out, size_t cap) {
  size_t declared = 0;
  if (!r.str(out, cap, &declared)) {
    sendError(seq, E_BAD_FRAME, "bad frame");
    return false;
  }
  if (strlen(out) != declared) {
    sendError(seq, E_PATH_REJECTED, "path rejected");
    return false;
  }
  return true;
}

// The expensive path: hash a file by reading it. Only reached when the digest
// was NOT already cached by the GET that streamed it -- which in practice means
// a redock resuming an interrupted pull. That is exactly why DELETE gets 60 s.
bool hashFile(const char *path, uint8_t out[32]) {
  sha256_ctx c;
  sha256_init(&c);
  uint8_t buf[512];
  uint32_t off = 0;
  while (true) {
    const int n = P().readAt(path, off, buf, sizeof(buf));
    if (n < 0) return false;
    if (n == 0) break;
    sha256_update(&c, buf, (size_t)n);
    off += (uint32_t)n;
  }
  sha256_final(&c, out);
  return true;
}

// --- commands ---------------------------------------------------------------

void doHello(uint8_t seq) {
  uint8_t pl[96];
  Writer w(pl, sizeof(pl));
  w.u16v(P().protoVersion());
  w.str(P().product());
  w.str(P().fwVersion());
  w.u8v(P().sdMounted() ? 1 : 0);
  w.u64v(P().clockEpoch());
  w.u8v(P().clockQuality());
  w.u8v(P().clockSetThisBoot() ? 1 : 0);
  w.u16v(MAX_PAYLOAD);

  // bit0: a capture WAS running when we docked. bit1: it is suspended right now.
  // Suspending nothing is not a suspension -- if no capture was running, both
  // bits stay clear. Prime prints this; the cost of docking is stated, never
  // silent.
  uint8_t flags = 0;
  if (P().loggingWasActive()) flags |= 0x01;
  if (P().loggingSuspended()) flags |= 0x02;
  w.u8v(flags);

  if (!w.ok()) { sendError(seq, E_IO, "io error"); return; }
  sendFrame(T_HELLO, seq, pl, (uint16_t)w.size());
}

void doSetClock(uint8_t seq, Reader &r) {
  const uint64_t epoch = r.u64v();
  const uint8_t quality = r.u8v();
  if (!r.ok()) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  // §4: Prime MUST NOT send quality=unsynced with a nonzero epoch. ENFORCED
  // here rather than trusted. A bad clock is labelled, never laundered into the
  // black box as truth -- and that guarantee cannot rest on Prime's manners.
  if (quality == Q_UNSYNCED && epoch != 0) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }
  if (quality > Q_NTP) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  P().setClock(epoch, quality);

  uint8_t pl[9];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  w.u64v(epoch);
  sendFrame(T_SET_CLOCK, seq, pl, (uint16_t)w.size());
}

void doList(uint8_t seq, Reader &r) {
  char dir[80];
  if (!readPath(seq, r, dir, sizeof(dir))) return;
  const uint8_t wantHashes = r.u8v();
  const uint16_t startIndex = r.u16v();
  if (!r.ok()) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(dir, Op::List);
  if (why) { sendError(seq, E_PATH_REJECTED, "path rejected"); return; }
  if (!P().sdMounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  DirEnt ents[MAX_ENTRIES];
  bool more = false;
  const int got = P().listDir(dir, startIndex, ents, MAX_ENTRIES, &more);
  if (got < 0) { sendError(seq, E_NOT_FOUND, "not found"); return; }

  uint8_t pl[MAX_PAYLOAD];
  Writer w(pl, sizeof(pl) - 1); // reserve the trailing `more` byte
  w.u16v(startIndex);
  w.u16v(0); // count, patched once we know how many actually fit

  uint16_t count = 0;
  for (int i = 0; i < got; ++i) {
    const size_t need = 1 + strlen(ents[i].name) + 4 + 8 + 1 + (wantHashes ? 32 : 0);
    if (!w.fits(need)) {
      // Out of room. Stop cleanly and tell Prime to come back. An oversized
      // reply is NOT a soft failure -- §2 makes Prime discard it, so the sync
      // would break exactly when the card holds the most to rescue.
      more = true;
      break;
    }
    w.str(ents[i].name);
    w.u32v(ents[i].size);
    w.u64v(0); // mtime: advisory only, and meaningless on a clock-less card
    if (wantHashes) {
      char full[80];
      snprintf(full, sizeof(full), "%s%s", dir, ents[i].name);
      uint8_t dg[32];
      if (hashFile(full, dg)) {
        w.u8v(1);
        w.bytes(dg, 32);
      } else {
        w.u8v(0); // Light MAY decline to hash; Prime falls back to name+size
      }
    } else {
      w.u8v(0);
    }
    count++;
  }
  if (!w.ok()) { sendError(seq, E_IO, "io error"); return; }

  pl[2] = (uint8_t)(count & 0xFF);
  pl[3] = (uint8_t)(count >> 8);
  size_t n = w.size();
  pl[n++] = more ? 1 : 0;
  sendFrame(T_LIST, seq, pl, (uint16_t)n);
}

void doPut(uint8_t seq, Reader &r) {
  char path[80];
  if (!readPath(seq, r, path, sizeof(path))) return;
  const uint32_t offset = r.u32v();
  const uint16_t chunkLen = r.u16v();
  if (!r.ok() || chunkLen > MAX_PAYLOAD) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Write);
  if (why) { sendError(seq, E_PATH_REJECTED, "path rejected"); return; }
  if (!P().sdMounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  uint8_t chunk[MAX_PAYLOAD];
  if (chunkLen && !r.bytes(chunk, chunkLen)) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  // Writes are STAGED, never live. The destination does not exist -- and is not
  // touched -- until COMMIT verifies the hash and renames. No commit, no file.
  char staging[88];
  stagingPathFor(path, staging, sizeof(staging));

  if (offset == 0) {
    if (!P().truncate(staging)) { sendError(seq, E_IO, "io error"); return; }
  } else {
    // The expected offset is the staging file's ACTUAL size, not something we
    // remember from earlier in this session. That matters: a dock interrupted
    // mid-PUT must be resumable on redock, and the next session has no memory
    // of the last. The file on the card is the only honest record of progress.
    const uint32_t staged = P().fileSize(staging);
    if (!P().exists(staging) || offset != staged) {
      // Chunks MUST be ascending and contiguous. A gap would leave a hole we
      // would then cheerfully hash and commit.
      sendError(seq, E_IO, "io error");
      return;
    }
  }

  if (chunkLen && P().appendTo(staging, chunk, chunkLen) != (int)chunkLen) {
    sendError(seq, E_IO, "io error");
    return;
  }

  uint8_t pl[5];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  w.u32v(P().fileSize(staging));
  sendFrame(T_PUT, seq, pl, (uint16_t)w.size());
}

void doCommit(uint8_t seq, Reader &r) {
  char path[80];
  if (!readPath(seq, r, path, sizeof(path))) return;
  uint8_t want[32];
  if (!r.bytes(want, 32)) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Write);
  if (why) { sendError(seq, E_PATH_REJECTED, "path rejected"); return; }
  if (!P().sdMounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  char staging[88];
  stagingPathFor(path, staging, sizeof(staging));

  // COMMIT with nothing staged: Prime hits this when a redock retries a COMMIT
  // whose PUTs never ran.
  if (!P().exists(staging)) { sendError(seq, E_NOT_FOUND, "not found"); return; }

  uint8_t got[32];
  if (!hashFile(staging, got)) { sendError(seq, E_IO, "io error"); return; }

  if (memcmp(got, want, 32) != 0) {
    // Verify BEFORE rename, never after. A staged file that does not hash is not
    // a file, it is noise -- unlink it rather than leave it to be retried.
    P().remove(staging);
    sendError(seq, E_HASH_MISMATCH, "hash mismatch");
    return;
  }

  // Prime always wins: overwrite, never merge.
  if (P().exists(path)) P().remove(path);
  if (!P().rename(staging, path)) { sendError(seq, E_IO, "io error"); return; }

  char line[128];
  snprintf(line, sizeof(line), "[dock] committed %s", path);
  P().diag(line);

  uint8_t pl[1];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  sendFrame(T_COMMIT, seq, pl, (uint16_t)w.size());
}

void doGet(uint8_t seq, Reader &r) {
  char path[80];
  if (!readPath(seq, r, path, sizeof(path))) return;
  const uint32_t offset = r.u32v();
  uint16_t maxLen = r.u16v();
  if (!r.ok()) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Read);
  if (why) { sendError(seq, E_PATH_REJECTED, "path rejected"); return; }

  // Validate the REQUEST before touching the card. A chunk larger than the
  // max_payload we advertised in HELLO is REFUSED, not quietly clamped --
  // clamping papers over a Prime that ignored our negotiated buffer and hands
  // back a chunk it never asked for. The frame itself is small and well-formed,
  // so this is ERR_TOO_LARGE, not ERR_BAD_FRAME.
  //
  // This check sits BEFORE exists(): an over-limit request is wrong whether or
  // not the file happens to be there, and Light should not do I/O for a request
  // it has already decided to refuse. (Caught on hardware -- the vector's
  // fixture file exists, so a probe naming a missing file got ERR_NOT_FOUND
  // instead, and the ordering never showed up in the shim.)
  if (maxLen > MAX_PAYLOAD) { sendError(seq, E_TOO_LARGE, "too large"); return; }

  if (!P().sdMounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }

  // The logger's CURRENT file is never served (§6). §6 closes it on the first
  // framed request, so this is normally unreachable -- but the black box wears
  // both belt and braces.
  const char *openPath = P().activeLogPath();
  if (openPath && strcmp(openPath, path) == 0) { sendError(seq, E_BUSY, "busy"); return; }

  if (!P().exists(path)) { sendError(seq, E_NOT_FOUND, "not found"); return; }
  const uint32_t size = P().fileSize(path);

  // Within the limit we may still return fewer bytes than asked: the response
  // envelope is u32 offset + u16 len + data + u8 eof, so the data we can carry
  // is MAX_PAYLOAD minus those 7 bytes. Prime advances by the `len` we report,
  // never by the `max_len` it requested.
  constexpr uint16_t ENVELOPE = 4 + 2 + 1;
  if (maxLen > MAX_PAYLOAD - ENVELOPE) maxLen = MAX_PAYLOAD - ENVELOPE;

  if (offset == 0) {
    sha256_init(&s_getCtx);
    snprintf(s_getPath, sizeof(s_getPath), "%s", path);
    s_getNext = 0;
    s_getHashing = true;
  } else if (!s_getHashing || strcmp(s_getPath, path) != 0 || offset != s_getNext) {
    // Random access cannot be folded into a running digest. Abandon the
    // streaming hash; DELETE will fall back to re-reading the file.
    s_getHashing = false;
  }

  uint16_t n = 0;
  uint8_t data[MAX_PAYLOAD];
  if (offset < size) {
    const uint32_t remain = size - offset;
    const uint16_t want = (uint16_t)((remain < maxLen) ? remain : maxLen);
    const int got = P().readAt(path, offset, data, want);
    if (got < 0) { sendError(seq, E_IO, "io error"); return; }
    n = (uint16_t)got;
    if (s_getHashing) {
      sha256_update(&s_getCtx, data, n);
      s_getNext = offset + n;
    }
  }
  // An offset at or past EOF is benign: len=0, eof=1, not an error. That keeps a
  // re-pull after an interrupted dock free of edge cases.

  // "Reaches" end of file, not "passes" it: when offset+len == size we say eof
  // even though len == max_len. Saves Prime a zero-byte round trip and makes the
  // progress math exact.
  const uint8_t eof = (offset + n >= size) ? 1 : 0;

  if (eof && s_getHashing && s_getNext == size) {
    snprintf(s_cache.path, sizeof(s_cache.path), "%s", path);
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
  if (!w.ok()) { sendError(seq, E_IO, "io error"); return; }
  sendFrame(T_GET, seq, pl, (uint16_t)w.size());
}

void doDelete(uint8_t seq, Reader &r) {
  char path[80];
  if (!readPath(seq, r, path, sizeof(path))) return;
  uint8_t want[32];
  if (!r.bytes(want, 32)) { sendError(seq, E_BAD_FRAME, "bad frame"); return; }

  const char *why = rejectReason(path, Op::Delete);
  if (why) { sendError(seq, E_PATH_REJECTED, "path rejected"); return; }
  if (!P().sdMounted()) { sendError(seq, E_NO_SD, "no sd card"); return; }
  if (!P().exists(path)) { sendError(seq, E_NOT_FOUND, "not found"); return; }

  // THE SCARY STEP, and deliberately the most defended thing here. A pull that
  // verifies nothing is a copy; a delete that verifies nothing is data loss on a
  // black box. We unlink on a hash match and on nothing else.
  uint8_t got[32];
  if (s_cache.valid && strcmp(s_cache.path, path) == 0) {
    memcpy(got, s_cache.digest, 32); // hashed as we streamed it -- free
  } else if (!hashFile(path, got)) {
    sendError(seq, E_IO, "io error");
    return;
  }

  if (memcmp(got, want, 32) != 0) { sendError(seq, E_HASH_MISMATCH, "hash mismatch"); return; }
  if (!P().remove(path)) { sendError(seq, E_IO, "io error"); return; }

  if (s_cache.valid && strcmp(s_cache.path, path) == 0) s_cache.valid = false;

  char line[128];
  snprintf(line, sizeof(line), "[dock] deleted (verified) %s", path);
  P().diag(line);

  uint8_t pl[1];
  Writer w(pl, sizeof(pl));
  w.u8v(1);
  sendFrame(T_DELETE, seq, pl, (uint16_t)w.size());
}

void endSession(const char *reason) {
  if (!s_active) return;
  s_active = false;
  s_getHashing = false;
  s_cache.valid = false;
  s_rxN = 0;

  char line[96];
  snprintf(line, sizeof(line), "[dock] session ended (%s) -- standalone", reason);
  P().diag(line);

  P().resumeLogging(); // a yanked cable must NEVER leave the black box off
}

void dispatch(const Frame &f) {
  Reader r(f.payload, f.len);

  // §8: on a version mismatch everything outside the every-version set (HELLO,
  // SET_CLOCK, BYE) is refused. A well-behaved Prime degrades on its own and
  // never gets here; this is the backstop for one that does not.
  const bool everyVersion = (f.type == T_HELLO || f.type == T_SET_CLOCK || f.type == T_BYE);
  if (!everyVersion && P().protoVersion() != PROTO_VERSION) {
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
    // is the line that holds the dock to provisioning + retrieval.
    sendError(f.seq, E_UNKNOWN_TYPE, "unknown type");
    break;
  }
}

void beginSession() {
  if (s_active) return;
  s_active = true;
  P().diag("[dock] framed traffic -- dock owns the port");
  // Take the file handle away from the logger BEFORE serving anything. One open
  // file at a time is not a preference, it is the FS layer's hard limit.
  P().suspendLogging();
}

} // namespace

// --- public -----------------------------------------------------------------

void setPlatform(Platform *p) { s_plat = p; }
Platform *platform() { return s_plat; }

void reset() {
  s_active = false;
  s_rxN = 0;
  s_lastFrameMs = 0;
  s_getHashing = false;
  s_getNext = 0;
  s_getPath[0] = '\0';
  s_cache.valid = false;
  s_cache.path[0] = '\0';
}

bool active() { return s_active; }
bool loggingSuspended() { return s_plat && P().loggingSuspended(); }

void tick(uint32_t nowMs) {
  if (!s_plat) return;

  // Drain the port. This MUST run before input::poll(), which consumes any
  // available byte and SWALLOWS the ones it does not recognise -- pointed at a
  // frame stream that is not a collision, it is a shredder.
  //
  // The door locks BOTH ways, though, and for a while it did not. Draining
  // unconditionally meant the dock ate the test hook's keystrokes too: parse()
  // discards everything ahead of a SOF, so a keypress survived only if it
  // happened to arrive in the window between this loop going dry and
  // input::poll() looking. Most did not. So: while no session is up, a byte
  // that cannot BEGIN a frame is not ours. Hand it back instead of shredding it.
  while (s_rxN < RX_CAP) {
    const int c = P().readByte();
    if (c < 0) break;
    if (!s_active && s_rxN == 0 && (uint8_t)c != SOF) {
      if (s_strayN < sizeof(s_stray)) s_stray[s_strayN++] = (uint8_t)c;
      continue;
    }
    s_rx[s_rxN++] = (uint8_t)c;
  }

  // Buffer full and still nothing parseable in it: that is garbage, not a frame.
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
      // Complete, in range, and corrupt. Reply -- echoing a SEQ we cannot vouch
      // for, which Prime treats as advisory -- then resync one byte on.
      beginSession();
      s_lastFrameMs = nowMs;
      sendError(f.seq, E_BAD_CRC, "bad crc");
      pos += used;
      continue;
    }

    pos += used; // Parse::Discard -- silence, by design (§2)
  }

  if (pos > 0) {
    memmove(s_rx, s_rx + pos, s_rxN - pos);
    s_rxN -= pos;
  }

  // The 10 s watchdog: the ONE place Light acts without being asked. It exists
  // to protect the black box -- a yanked cable mid-session must not leave
  // logging switched off forever.
  if (s_active && (nowMs - s_lastFrameMs) > WATCHDOG_MS) {
    endSession("watchdog -- no frame in 10s");
  }
}

int takeStray() {
  if (s_strayN == 0) return -1;
  const int c = s_stray[0];
  memmove(s_stray, s_stray + 1, --s_strayN);
  return c;
}

} // namespace dock
