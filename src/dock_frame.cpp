// Dock frame codec + payload (de)serialisation. Pure: no Arduino, no SD.
// Runs natively against the conformance vectors -- see test/test_dock/.

#include "dock.h"

#include <string.h>

namespace dock {

uint16_t crc16(const uint8_t *data, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    c ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; ++b) {
      c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
  }
  return c;
}

Parse parse(const uint8_t *buf, size_t len, Frame &out, size_t &consumed) {
  consumed = 0;

  // Drop everything before the first SOF. Garbage on the wire is expected --
  // a serial monitor was open, the cable glitched, someone typed.
  size_t i = 0;
  while (i < len && buf[i] != SOF) ++i;
  if (i > 0) {
    consumed = i;
    return Parse::Discard;
  }
  if (len < 5) return Parse::NeedMore; // need SOF+TYPE+SEQ+LEN before we know more

  const uint8_t type = buf[1];
  const uint8_t seq = buf[2];
  const uint16_t plen = (uint16_t)(buf[3] | ((uint16_t)buf[4] << 8));

  // Never allocate or block on an unvalidated LEN (§2). A header claiming more
  // than max_payload is a false SOF, not a frame: discard ONE byte and rescan,
  // and say nothing -- it never passed CRC, so it has no SEQ we may echo.
  if (plen > MAX_PAYLOAD) {
    consumed = 1;
    return Parse::Discard;
  }

  const size_t total = (size_t)FRAME_OVERHEAD + plen;
  if (len < total) return Parse::NeedMore; // rest of it may still be in flight

  const uint16_t rx = (uint16_t)(buf[5 + plen] | ((uint16_t)buf[6 + plen] << 8));
  if (crc16(&buf[1], (size_t)4 + plen) != rx) {
    // Syntactically complete, in-range, and corrupt. Reply ERR_BAD_CRC echoing
    // what we received -- advisory only, since these bytes come from a frame we
    // just proved untrustworthy -- then resync one byte on per §2.
    out.type = type;
    out.seq = seq;
    out.len = 0;
    out.payload = nullptr;
    consumed = 1;
    return Parse::BadCrc;
  }

  out.type = type;
  out.seq = seq;
  out.len = plen;
  out.payload = &buf[5];
  consumed = total;
  return Parse::Ok;
}

size_t encode(uint8_t type, uint8_t seq, const uint8_t *payload,
              uint16_t payloadLen, uint8_t *out, size_t outCap) {
  if (payloadLen > MAX_PAYLOAD) return 0;
  const size_t total = (size_t)FRAME_OVERHEAD + payloadLen;
  if (outCap < total) return 0;

  out[0] = SOF;
  out[1] = type;
  out[2] = seq;
  out[3] = (uint8_t)(payloadLen & 0xFF);
  out[4] = (uint8_t)(payloadLen >> 8);
  if (payloadLen && payload) memcpy(&out[5], payload, payloadLen);

  const uint16_t c = crc16(&out[1], (size_t)4 + payloadLen);
  out[5 + payloadLen] = (uint8_t)(c & 0xFF);
  out[6 + payloadLen] = (uint8_t)(c >> 8);
  return total;
}

// --- Writer -----------------------------------------------------------------

void Writer::u8v(uint8_t v) {
  if (!fits(1)) { ok_ = false; return; }
  b_[n_++] = v;
}

void Writer::u16v(uint16_t v) {
  if (!fits(2)) { ok_ = false; return; }
  b_[n_++] = (uint8_t)(v & 0xFF);
  b_[n_++] = (uint8_t)(v >> 8);
}

void Writer::u32v(uint32_t v) {
  if (!fits(4)) { ok_ = false; return; }
  for (uint8_t i = 0; i < 4; ++i) b_[n_++] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

void Writer::u64v(uint64_t v) {
  if (!fits(8)) { ok_ = false; return; }
  for (uint8_t i = 0; i < 8; ++i) b_[n_++] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

void Writer::bytes(const uint8_t *p, size_t n) {
  if (!fits(n)) { ok_ = false; return; }
  memcpy(&b_[n_], p, n);
  n_ += n;
}

void Writer::str(const char *s) {
  const size_t n = s ? strlen(s) : 0;
  if (n > 255) { ok_ = false; return; }
  if (!fits(1 + n)) { ok_ = false; return; }
  b_[n_++] = (uint8_t)n;
  if (n) memcpy(&b_[n_], s, n);
  n_ += n;
}

// --- Reader -----------------------------------------------------------------

uint8_t Reader::u8v() {
  if (i_ + 1 > len_) { ok_ = false; return 0; }
  return b_[i_++];
}

uint16_t Reader::u16v() {
  if (i_ + 2 > len_) { ok_ = false; i_ = len_; return 0; }
  const uint16_t v = (uint16_t)(b_[i_] | ((uint16_t)b_[i_ + 1] << 8));
  i_ += 2;
  return v;
}

uint32_t Reader::u32v() {
  if (i_ + 4 > len_) { ok_ = false; i_ = len_; return 0; }
  uint32_t v = 0;
  for (uint8_t k = 0; k < 4; ++k) v |= (uint32_t)b_[i_ + k] << (8 * k);
  i_ += 4;
  return v;
}

uint64_t Reader::u64v() {
  if (i_ + 8 > len_) { ok_ = false; i_ = len_; return 0; }
  uint64_t v = 0;
  for (uint8_t k = 0; k < 8; ++k) v |= (uint64_t)b_[i_ + k] << (8 * k);
  i_ += 8;
  return v;
}

bool Reader::bytes(uint8_t *out, size_t n) {
  if (i_ + n > len_) { ok_ = false; i_ = len_; return false; }
  memcpy(out, &b_[i_], n);
  i_ += n;
  return true;
}

bool Reader::str(char *out, size_t outCap, size_t *declaredLen) {
  if (i_ + 1 > len_) { ok_ = false; return false; }
  const uint8_t n = b_[i_++];
  if (declaredLen) *declaredLen = n;
  if (i_ + n > len_) { ok_ = false; i_ = len_; return false; }
  // A string longer than the destination is malformed, not truncatable: paths
  // are bounded and silently clipping one would change which file we touch.
  if ((size_t)n + 1 > outCap) { ok_ = false; i_ += n; return false; }
  memcpy(out, &b_[i_], n);
  out[n] = '\0';
  i_ += n;
  return true;
}

} // namespace dock
