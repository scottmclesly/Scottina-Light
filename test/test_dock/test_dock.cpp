// Light Dock conformance + unit tests. Runs natively -- no board, no SD:
//
//     pio test -e native
//
// This is the whole point of DOCK-PROTOCOL.md §10. The vectors are the shared
// asset; Light tests its codec against them here, Prime tests its engine against
// a fake Light that replays them, and neither lane waits on the other.

#include <string.h>
#include <unity.h>

#include "../../include/dock.h"
#include "../../lib/sha256/sha256.h"
#include "vectors_generated.h"

using namespace dock;

// --- helpers ----------------------------------------------------------------

static void hexToBytes(const char *hex, uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    auto nib = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
      if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
      return (uint8_t)(c - 'A' + 10);
    };
    out[i] = (uint8_t)((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
  }
}

static const Vec *vec(const char *id) {
  for (size_t i = 0; i < N_VECTORS; ++i)
    if (strcmp(VECTORS[i].id, id) == 0) return &VECTORS[i];
  return nullptr;
}

// --- framing ----------------------------------------------------------------

// The one number both implementations must agree on before anything else can be
// trusted. DOCK-PROTOCOL.md §2 states it explicitly for exactly this reason.
void test_crc_check_value(void) {
  const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(s, sizeof(s)));
}

void test_sha256_known_answers(void) {
  uint8_t d[32];
  sha256_ctx c;

  // FIPS 180-4 "abc"
  sha256_init(&c);
  sha256_update(&c, (const uint8_t *)"abc", 3);
  sha256_final(&c, d);
  uint8_t want[32];
  hexToBytes("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 32);

  // Empty input -- the boundary the padding code most often gets wrong.
  sha256_init(&c);
  sha256_final(&c, d);
  hexToBytes("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 32);

  // The digest baked into dock-vectors.json for /logs/raw001.log. If this fails,
  // Light and the vectors disagree about what a file's identity even is.
  sha256_init(&c);
  sha256_update(&c, (const uint8_t *)"hello black box\n", 16);
  sha256_final(&c, d);
  hexToBytes("8046883a45287e8020ad0ae66f450f7ad99a629210f7d3670627e490d0336135", want, 32);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d, 32);
}

// Every wire_out in the vector file must parse cleanly AND re-encode to exactly
// the bytes we were given. This exercises decode and encode against each other
// over all 47 vectors, so a byte-order or CRC slip cannot hide.
void test_all_vector_frames_roundtrip(void) {
  size_t checked = 0;
  for (size_t i = 0; i < N_VECTORS; ++i) {
    const Vec &v = VECTORS[i];
    for (size_t j = 0; j < v.n_outs; ++j) {
      const VecOut &o = v.outs[j];
      Frame f;
      size_t used = 0;
      const Parse p = parse(o.bytes, o.len, f, used);

      TEST_ASSERT_EQUAL_MESSAGE((int)Parse::Ok, (int)p, v.id);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(o.len, used, v.id);
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(o.type, f.type, v.id);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(o.seq, f.seq, v.id);

      uint8_t re[MAX_PAYLOAD + FRAME_OVERHEAD];
      const size_t n = encode(f.type, f.seq, f.payload, f.len, re, sizeof(re));
      TEST_ASSERT_EQUAL_size_t_MESSAGE(o.len, n, v.id);
      TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(o.bytes, re, n, v.id);
      ++checked;
    }
  }
  TEST_ASSERT_GREATER_THAN_size_t(45, checked);
}

// PIN (err-bad-crc): a syntactically complete, in-range frame that fails CRC
// gets ERR_BAD_CRC echoing the RECEIVED seq -- advisory only, since it comes
// from a frame we just proved untrustworthy -- and then resyncs one byte on.
void test_pin_bad_crc_replies_and_resyncs(void) {
  const Vec *v = vec("err-bad-crc");
  TEST_ASSERT_NOT_NULL(v);

  Frame f;
  size_t used = 0;
  const Parse p = parse(v->wire_in, v->wire_in_len, f, used);

  TEST_ASSERT_EQUAL((int)Parse::BadCrc, (int)p);
  TEST_ASSERT_EQUAL_UINT8(18, f.seq);   // the advisory echo Prime will log
  TEST_ASSERT_EQUAL_size_t(1, used);    // discard exactly one byte, then rescan
}

// PIN (resync-false-sof-huge-len): a false SOF whose LEN exceeds max_payload is
// discarded ONE byte at a time in SILENCE -- it never passed CRC, so it has no
// seq we may echo -- and the real frame behind it is answered normally.
// Never allocate or block on an unvalidated LEN.
void test_pin_huge_len_is_silent_then_recovers(void) {
  const Vec *v = vec("resync-false-sof-huge-len");
  TEST_ASSERT_NOT_NULL(v);

  const uint8_t *buf = v->wire_in;
  size_t len = v->wire_in_len, used = 0;
  Frame f;

  // First: the false SOF. Discarded, no frame emitted.
  Parse p = parse(buf, len, f, used);
  TEST_ASSERT_EQUAL((int)Parse::Discard, (int)p);
  TEST_ASSERT_EQUAL_size_t(1, used);

  // Keep feeding until the genuine HELLO surfaces. It must, and it must be intact.
  int guard = 0;
  do {
    buf += used;
    len -= used;
    p = parse(buf, len, f, used);
    TEST_ASSERT_TRUE_MESSAGE(++guard < 16, "resync failed to converge");
  } while (p == Parse::Discard);

  TEST_ASSERT_EQUAL((int)Parse::Ok, (int)p);
  TEST_ASSERT_EQUAL_HEX8(T_HELLO, f.type);
  TEST_ASSERT_EQUAL_UINT8(41, f.seq);
}

// An incomplete frame is NeedMore, never a guess: consume nothing and wait. The
// 10 s watchdog (§5), not the parser, is what resolves a cable that went away.
void test_truncated_frame_is_needmore(void) {
  const Vec *v = vec("truncated-frame-then-silence");
  TEST_ASSERT_NOT_NULL(v);

  Frame f;
  size_t used = 0;
  TEST_ASSERT_EQUAL((int)Parse::NeedMore,
                    (int)parse(v->wire_in, v->wire_in_len, f, used));
  TEST_ASSERT_EQUAL_size_t(0, used);
}

// Two requests arriving in one read must both come out, with their seqs intact.
void test_back_to_back_frames(void) {
  const Vec *v = vec("seq-echo-back-to-back");
  TEST_ASSERT_NOT_NULL(v);

  const uint8_t *buf = v->wire_in;
  size_t len = v->wire_in_len, used = 0;
  Frame f;

  TEST_ASSERT_EQUAL((int)Parse::Ok, (int)parse(buf, len, f, used));
  TEST_ASSERT_EQUAL_UINT8(44, f.seq);
  buf += used;
  len -= used;
  TEST_ASSERT_EQUAL((int)Parse::Ok, (int)parse(buf, len, f, used));
  TEST_ASSERT_EQUAL_UINT8(45, f.seq);
}

// A LEN that overruns MAX_PAYLOAD must never be encodable either.
void test_encode_refuses_oversized_payload(void) {
  uint8_t out[8];
  TEST_ASSERT_EQUAL_size_t(0, encode(T_HELLO, 1, nullptr, MAX_PAYLOAD + 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_size_t(0, encode(T_HELLO, 1, nullptr, 0, out, 3)); // no room
}

// --- LIST pagination (added pre-ratification; DOCK-PROTOCOL.md §4) ----------
//
// An entry costs 24 B unhashed, so ~42 logs fill max_payload=1024. Before
// pagination existed the reply simply overflowed, and §2 made Prime DISCARD it
// -- the sync broke exactly when the card held the most to rescue.

void test_list_pages_fit_max_payload(void) {
  for (size_t i = 0; i < N_VECTORS; ++i) {
    const Vec &v = VECTORS[i];
    for (size_t j = 0; j < v.n_outs; ++j) {
      Frame f;
      size_t used = 0;
      TEST_ASSERT_EQUAL((int)Parse::Ok, (int)parse(v.outs[j].bytes, v.outs[j].len, f, used));
      TEST_ASSERT_TRUE_MESSAGE(f.len <= MAX_PAYLOAD, v.id);
    }
  }
}

void test_list_pagination_walks_to_completion(void) {
  const Vec *p1 = vec("list-logs-paginated");
  const Vec *p2 = vec("list-logs-paginated-page2");
  TEST_ASSERT_NOT_NULL(p1);
  TEST_ASSERT_NOT_NULL(p2);

  auto decodePage = [](const Vec *v, uint16_t &start, uint16_t &count, uint8_t &more) {
    Frame f;
    size_t used = 0;
    TEST_ASSERT_EQUAL((int)Parse::Ok, (int)parse(v->outs[0].bytes, v->outs[0].len, f, used));
    Reader r(f.payload, f.len);
    start = r.u16v();
    count = r.u16v();
    for (uint16_t k = 0; k < count; ++k) { // skip entries
      char name[64];
      TEST_ASSERT_TRUE(r.str(name, sizeof(name)));
      r.u32v();
      r.u64v();
      if (r.u8v()) {
        uint8_t sha[32];
        r.bytes(sha, 32);
      }
    }
    more = r.u8v();
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE_MESSAGE(r.done(), "trailing bytes in LIST payload");
  };

  uint16_t s1, c1, s2, c2;
  uint8_t m1, m2;
  decodePage(p1, s1, c1, m1);
  decodePage(p2, s2, c2, m2);

  TEST_ASSERT_EQUAL_UINT16(0, s1);
  TEST_ASSERT_EQUAL_UINT8(1, m1);          // more entries remain
  TEST_ASSERT_EQUAL_UINT16(s1 + c1, s2);   // Prime advances start_index by count
  TEST_ASSERT_EQUAL_UINT8(0, m2);          // and the walk terminates
  TEST_ASSERT_EQUAL_UINT16(45, c1 + c2);   // all 45 logs accounted for, none twice
}

// PIN (list-start-index-past-end): benign, exactly like GET past EOF.
void test_list_start_index_past_end_is_benign(void) {
  const Vec *v = vec("list-start-index-past-end");
  TEST_ASSERT_NOT_NULL(v);

  Frame f;
  size_t used = 0;
  TEST_ASSERT_EQUAL((int)Parse::Ok, (int)parse(v->outs[0].bytes, v->outs[0].len, f, used));
  TEST_ASSERT_EQUAL_HEX8(T_LIST, f.type); // NOT an error frame

  Reader r(f.payload, f.len);
  TEST_ASSERT_EQUAL_UINT16(99, r.u16v()); // start_index echoed
  TEST_ASSERT_EQUAL_UINT16(0, r.u16v());  // count
  TEST_ASSERT_EQUAL_UINT8(0, r.u8v());    // more
  TEST_ASSERT_TRUE(r.done());
}

// --- the reject pass (§7) -- one test per rejected class --------------------

void test_reject_path_traversal(void) {
  TEST_ASSERT_FALSE(pathAllowed("/logs/../config/i2c.json", Op::Read));
  TEST_ASSERT_FALSE(pathAllowed("/tables/../../etc/passwd", Op::Write));
  TEST_ASSERT_FALSE(pathAllowed("/..", Op::List));
  // ".." as a SEGMENT, not as a substring: this is a legal filename.
  TEST_ASSERT_TRUE(pathAllowed("/logs/a..b.log", Op::Read));
}

void test_reject_write_to_logs(void) {
  // The black box is append-only from the outside. Nothing Prime sends lands here.
  TEST_ASSERT_FALSE(pathAllowed("/logs/raw000.log", Op::Write));
  TEST_ASSERT_FALSE(pathAllowed("/logs/evil.log", Op::Write));
  TEST_ASSERT_TRUE(pathAllowed("/logs/raw000.log", Op::Read));   // reads are fine
  TEST_ASSERT_TRUE(pathAllowed("/logs/raw000.log", Op::Delete)); // verified deletes are fine
}

void test_reject_tier2_config_read(void) {
  // Tier-2 is the user's own on-device choices. None of Prime's business.
  TEST_ASSERT_FALSE(pathAllowed("/config/i2c.json", Op::Read));
  TEST_ASSERT_FALSE(pathAllowed("/config/ui.json", Op::Read));
  TEST_ASSERT_FALSE(pathAllowed("/config/", Op::List));
  // Tier-3 is authored off-device, so Prime MAY write it.
  TEST_ASSERT_TRUE(pathAllowed("/config.json", Op::Write));
  TEST_ASSERT_FALSE(pathAllowed("/config.json", Op::Delete));
}

void test_reject_structural_junk(void) {
  TEST_ASSERT_FALSE(pathAllowed("logs/raw000.log", Op::Read));   // not rooted
  TEST_ASSERT_FALSE(pathAllowed("\\logs\\x.log", Op::Read));     // backslash
  TEST_ASSERT_FALSE(pathAllowed("/logs//x.log", Op::Read));      // empty segment
  TEST_ASSERT_FALSE(pathAllowed("", Op::Read));                  // empty
  TEST_ASSERT_FALSE(pathAllowed("/logs/x\x01.log", Op::Read));   // control byte
}

void test_allow_list_positive_cases(void) {
  TEST_ASSERT_TRUE(pathAllowed("/logs/", Op::List));
  TEST_ASSERT_TRUE(pathAllowed("/tables/", Op::List));
  TEST_ASSERT_TRUE(pathAllowed("/tables/nmea2000-std.json", Op::Write));
  TEST_ASSERT_TRUE(pathAllowed("/logs/raw000.log", Op::Read));
  // Tables are pushed, never pulled: Prime is their source of truth.
  TEST_ASSERT_FALSE(pathAllowed("/tables/nmea2000-std.json", Op::Read));
  TEST_ASSERT_FALSE(pathAllowed("/tables/nmea2000-std.json", Op::Delete));
}

// --- reader/writer hardening ------------------------------------------------

void test_reader_short_payload_fails_closed(void) {
  const uint8_t buf[2] = {0x01, 0x02};
  Reader r(buf, sizeof(buf));
  r.u32v(); // wants 4, has 2
  TEST_ASSERT_FALSE(r.ok());
}

void test_reader_overlong_string_fails_closed(void) {
  // A 10-byte string read into an 8-byte buffer is malformed, not truncatable:
  // silently clipping a path would change WHICH FILE we touch.
  const uint8_t buf[11] = {10, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};
  Reader r(buf, sizeof(buf));
  char small[8];
  TEST_ASSERT_FALSE(r.str(small, sizeof(small)));
  TEST_ASSERT_FALSE(r.ok());
}

void test_writer_overflow_latches(void) {
  uint8_t buf[4];
  Writer w(buf, sizeof(buf));
  w.u32v(0xDEADBEEF);
  TEST_ASSERT_TRUE(w.ok());
  w.u8v(0x01); // one byte too many
  TEST_ASSERT_FALSE(w.ok());
  TEST_ASSERT_EQUAL_size_t(4, w.size()); // and it did not scribble
}

// ----------------------------------------------------------------------------

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_crc_check_value);
  RUN_TEST(test_sha256_known_answers);
  RUN_TEST(test_all_vector_frames_roundtrip);
  RUN_TEST(test_pin_bad_crc_replies_and_resyncs);
  RUN_TEST(test_pin_huge_len_is_silent_then_recovers);
  RUN_TEST(test_truncated_frame_is_needmore);
  RUN_TEST(test_back_to_back_frames);
  RUN_TEST(test_encode_refuses_oversized_payload);
  RUN_TEST(test_list_pages_fit_max_payload);
  RUN_TEST(test_list_pagination_walks_to_completion);
  RUN_TEST(test_list_start_index_past_end_is_benign);
  RUN_TEST(test_reject_path_traversal);
  RUN_TEST(test_reject_write_to_logs);
  RUN_TEST(test_reject_tier2_config_read);
  RUN_TEST(test_reject_structural_junk);
  RUN_TEST(test_allow_list_positive_cases);
  RUN_TEST(test_reader_short_payload_fails_closed);
  RUN_TEST(test_reader_overlong_string_fails_closed);
  RUN_TEST(test_writer_overflow_latches);
  return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
