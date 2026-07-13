// Path policy -- DOCK-PROTOCOL.md §7. Pure: no Arduino, no SD.
//
// Two independent layers, and the order matters:
//
//   1. The REJECT PASS runs first and knows nothing about the allow-list. It
//      throws out anything structurally untrustworthy -- traversal, backslashes,
//      control bytes, an unrooted path -- before any prefix is even considered.
//   2. The ALLOW-LIST then answers the positive question: is this exact prefix
//      permitted for this exact operation?
//
// Either layer alone would pass the test suite. Both exist because this is the
// module that can unlink a black box, and defense in depth is cheap here.

#include "dock.h"

#include <string.h>

namespace dock {

namespace {

bool startsWith(const char *s, const char *prefix) {
  const size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

// Structural rejection, independent of any prefix. Returns nullptr if clean.
const char *structuralReject(const char *path) {
  if (!path || path[0] == '\0') return "empty path";
  if (path[0] != '/') return "not rooted at /";

  const size_t n = strlen(path);
  if (n > 127) return "path too long";

  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = (unsigned char)path[i];
    if (c == '\\') return "backslash";
    if (c < 0x20 || c == 0x7F) return "control byte";
    // "//" collapses differently across FAT layers; refuse rather than guess.
    if (c == '/' && i + 1 < n && path[i + 1] == '/') return "empty path segment";
  }

  // Traversal. Catch the segment "..", not the substring -- "/logs/a..b.log" is
  // a legal (if odd) filename and there is no reason to refuse it.
  const char *seg = path + 1;
  while (seg && *seg) {
    const char *end = strchr(seg, '/');
    const size_t len = end ? (size_t)(end - seg) : strlen(seg);
    if (len == 2 && seg[0] == '.' && seg[1] == '.') return "path traversal";
    if (len == 0) return "empty path segment";
    seg = end ? end + 1 : nullptr;
  }
  return nullptr;
}

} // namespace

const char *rejectReason(const char *path, Op op) {
  const char *s = structuralReject(path);
  if (s) return s;

  const bool inLogs = startsWith(path, "/logs/");
  const bool inTables = startsWith(path, "/tables/");
  const bool isTier3 = (strcmp(path, "/config.json") == 0);

  switch (op) {
  case Op::List:
    // Exactly the two roots, and only as directories.
    if (strcmp(path, "/logs/") == 0 || strcmp(path, "/tables/") == 0) return nullptr;
    // Tier-2 config is the user's own on-device choices. None of Prime's
    // business, and saying so explicitly beats letting the allow-list imply it.
    if (startsWith(path, "/config/")) return "Tier-2 config is not readable";
    return "not a listable root";

  case Op::Read:
    // GET serves logs only. Tables are pushed, never pulled -- Prime is the
    // source of truth for them, so reading one back has no meaning.
    if (inLogs) return nullptr;
    if (startsWith(path, "/config/")) return "Tier-2 config is not readable";
    return "reads are limited to /logs/";

  case Op::Write:
    if (inTables || isTier3) return nullptr;
    // The black box is append-only from the outside. Nothing Prime sends may
    // land in /logs/, ever.
    if (inLogs) return "writes to /logs/ are forbidden";
    return "writes are limited to /tables/ and /config.json";

  case Op::Delete:
    if (inLogs) return nullptr;
    return "deletes are limited to /logs/";
  }
  return "unknown operation";
}

bool pathAllowed(const char *path, Op op) { return rejectReason(path, op) == nullptr; }

} // namespace dock
