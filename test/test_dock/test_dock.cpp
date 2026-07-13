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
#include "../../include/dock_platform.h"
#include "../../lib/sha256/sha256.h"
#include "fake_platform.h"
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

// --- FULL PROTOCOL REPLAY ---------------------------------------------------
//
// The point of the shim. Every vector is stood up as a fake Light in exactly the
// state the vector describes, the wire_in bytes are fed to the real handler, and
// the bytes it writes back must equal the expected wire_out EXACTLY -- byte for
// byte, CRC and all. No hardware, no SD card, no board on the bench.
//
// This is what makes "proven before the devices meet" true of the whole protocol
// rather than only of the frame codec.

static FakePlatform *g_fake = nullptr;

static void standUp(const Vec &v) {
  delete g_fake;
  g_fake = new FakePlatform();

  for (size_t i = 0; i < v.n_files; ++i) {
    const VecFile &f = v.files[i];
    g_fake->files.push_back({f.path, std::vector<uint8_t>(f.data, f.data + f.size)});
  }
  g_fake->sd = (v.sd_present != 0);
  g_fake->proto = (uint16_t)v.proto_version;
  g_fake->epoch = v.clock_epoch;
  g_fake->qual = (uint8_t)v.clock_quality;
  g_fake->setBoot = (v.clock_set_this_boot != 0);
  g_fake->logWasActive = (v.logging_was_active != 0);
  if (v.open_path) g_fake->openPath = v.open_path;

  setPlatform(g_fake);
  reset();
}

void test_replay_all_vectors(void) {
  size_t replayed = 0;

  for (size_t i = 0; i < N_VECTORS; ++i) {
    const Vec &v = VECTORS[i];
    standUp(v);

    g_fake->give(v.wire_in, v.wire_in_len);
    // Two ticks at the same instant: the first drains and answers, the second
    // proves the handler is idempotent on an empty port and does not re-answer.
    tick(1000);
    tick(1000);

    // Concatenate what the vector says Light should have said.
    std::vector<uint8_t> want;
    for (size_t j = 0; j < v.n_outs; ++j)
      want.insert(want.end(), v.outs[j].bytes, v.outs[j].bytes + v.outs[j].len);

    TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), g_fake->tx.size(), v.id);
    if (!want.empty())
      TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want.data(), g_fake->tx.data(), want.size(), v.id);
    replayed++;
  }
  TEST_ASSERT_EQUAL_size_t(N_VECTORS, replayed);
}

// COMMIT is the whole interruption story: no commit, no file. Verify BEFORE the
// rename, never after -- a staged file that does not hash is not a file.
void test_commit_renames_only_on_hash_match(void) {
  const Vec *v = vec("commit");
  TEST_ASSERT_NOT_NULL(v);
  standUp(*v);

  TEST_ASSERT_TRUE(g_fake->has("/tables/nmea2000-std.json.partial"));
  g_fake->give(v->wire_in, v->wire_in_len);
  tick(1000);

  // Staged file is gone; the real one exists. Atomic rename, after the check.
  TEST_ASSERT_FALSE(g_fake->has("/tables/nmea2000-std.json.partial"));
  TEST_ASSERT_TRUE(g_fake->has("/tables/nmea2000-std.json"));
}

// A COMMIT whose hash does not match must not merely be refused -- it must leave
// the GOOD file that is already there completely untouched. A bad push is not
// allowed to become a corrupted table.
void test_commit_hash_mismatch_preserves_existing_file(void) {
  const Vec *v = vec("err-hash-mismatch-commit");
  TEST_ASSERT_NOT_NULL(v);
  standUp(*v);

  FakePlatform::FFile *before = g_fake->find("/tables/nmea2000-std.json");
  TEST_ASSERT_NOT_NULL_MESSAGE(before, "fixture should already hold a good table");
  const std::vector<uint8_t> original = before->data;

  g_fake->give(v->wire_in, v->wire_in_len);
  tick(1000);

  // The staged noise is unlinked -- it is not a file, and leaving it would only
  // invite a retry to commit it.
  TEST_ASSERT_FALSE(g_fake->has("/tables/nmea2000-std.json.partial"));

  // And the destination is byte-for-byte what it was.
  FakePlatform::FFile *after = g_fake->find("/tables/nmea2000-std.json");
  TEST_ASSERT_NOT_NULL_MESSAGE(after, "a failed COMMIT must not delete the good file");
  TEST_ASSERT_EQUAL_size_t(original.size(), after->data.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(original.data(), after->data.data(), original.size());
}

// A PUT resuming across a redock: 24 bytes are already staged and the next chunk
// arrives at offset 24 in a session that never saw offset 0. The expected offset
// must come from the FILE, not from anything we remember -- the card is the only
// honest record of how far a PUT got.
void test_put_resumes_from_staged_size_not_memory(void) {
  const Vec *v = vec("put-chunk-1");
  TEST_ASSERT_NOT_NULL(v);
  standUp(*v);

  TEST_ASSERT_EQUAL_UINT32(24, g_fake->fileSize("/tables/nmea2000-std.json.partial"));
  g_fake->give(v->wire_in, v->wire_in_len);
  tick(1000);

  TEST_ASSERT_EQUAL_UINT32(36, g_fake->fileSize("/tables/nmea2000-std.json.partial"));
}

// THE SCARY STEP. A delete that verifies nothing is data loss on a black box.
void test_delete_unlinks_only_on_verified_hash(void) {
  const Vec *ok = vec("delete-verified");
  const Vec *bad = vec("err-hash-mismatch-delete");
  TEST_ASSERT_NOT_NULL(ok);
  TEST_ASSERT_NOT_NULL(bad);

  standUp(*ok);
  g_fake->give(ok->wire_in, ok->wire_in_len);
  tick(1000);
  TEST_ASSERT_FALSE_MESSAGE(g_fake->has("/logs/raw001.log"), "verified delete should unlink");

  standUp(*bad);
  const bool before = g_fake->has("/logs/raw001.log");
  g_fake->give(bad->wire_in, bad->wire_in_len);
  tick(1000);
  TEST_ASSERT_TRUE_MESSAGE(before, "fixture should have the file");
  TEST_ASSERT_TRUE_MESSAGE(g_fake->has("/logs/raw001.log"),
                           "a delete whose hash does not match MUST NOT unlink");
}

// The reject pass, exercised through the wire rather than the API: a write aimed
// at /logs/ must be refused, and must not create anything.
void test_reject_pass_over_the_wire_creates_nothing(void) {
  const Vec *v = vec("reject-write-to-logs");
  TEST_ASSERT_NOT_NULL(v);
  standUp(*v);

  const size_t before = g_fake->files.size();
  g_fake->give(v->wire_in, v->wire_in_len);
  tick(1000);

  TEST_ASSERT_EQUAL_size_t_MESSAGE(before, g_fake->files.size(),
                                   "a rejected PUT must not stage anything");
}

// §6 + §5: a session that goes quiet must not leave the black box switched off.
// This is the one place Light acts without being asked, and it is why.
void test_watchdog_resumes_logging_after_silence(void) {
  const Vec *v = vec("hello");
  TEST_ASSERT_NOT_NULL(v);
  standUp(*v);

  g_fake->give(v->wire_in, v->wire_in_len);
  tick(1000);
  TEST_ASSERT_TRUE_MESSAGE(active(), "a valid frame should open a session");
  TEST_ASSERT_TRUE_MESSAGE(g_fake->logSuspended, "docking suspends logging");

  tick(1000 + 9000); // 9 s of silence -- not yet
  TEST_ASSERT_TRUE(active());

  tick(1000 + 10001); // past the 10 s watchdog
  TEST_ASSERT_FALSE_MESSAGE(active(), "watchdog must end the session");
  TEST_ASSERT_TRUE_MESSAGE(g_fake->logResumed, "watchdog MUST resume logging");
  TEST_ASSERT_FALSE(g_fake->logSuspended);
}

// BYE hands the port back and restarts the logger -- the ordinary way out.
void test_bye_resumes_logging(void) {
  const Vec *v = vec("bye");
  TEST_ASSERT_NOT_NULL(v);
  standUp(*v);

  g_fake->give(v->wire_in, v->wire_in_len);
  tick(1000);

  TEST_ASSERT_FALSE(active());
  TEST_ASSERT_TRUE(g_fake->logResumed);
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

  // Full protocol replay against the in-memory Light.
  RUN_TEST(test_replay_all_vectors);
  RUN_TEST(test_commit_renames_only_on_hash_match);
  RUN_TEST(test_commit_hash_mismatch_preserves_existing_file);
  RUN_TEST(test_put_resumes_from_staged_size_not_memory);
  RUN_TEST(test_delete_unlinks_only_on_verified_hash);
  RUN_TEST(test_reject_pass_over_the_wire_creates_nothing);
  RUN_TEST(test_watchdog_resumes_logging_after_silence);
  RUN_TEST(test_bye_resumes_logging);
  return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
