// An in-memory Light: dock::Platform backed by RAM instead of a Wio Terminal.
//
// This is what lets the ENTIRE protocol -- paging, staging, hashing, the reject
// pass, the watchdog, the delete-verify -- be proven before the two devices ever
// meet on a bench. Prime tests its engine against a fake Light replaying the
// vectors; this is the same trick pointed the other way.
//
// It deliberately mimics the real FS's awkward parts rather than an idealised
// filesystem: entries come back in lexicographic order (matching the fake Light
// the vectors were authored against), and a file held open is genuinely busy.

#ifndef SL_FAKE_PLATFORM_H
#define SL_FAKE_PLATFORM_H

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../include/dock_platform.h"

class FakePlatform : public dock::Platform {
public:
  // --- the fake card ---
  struct FFile {
    std::string path;
    std::vector<uint8_t> data;
  };
  std::vector<FFile> files;

  bool sd = true;
  uint16_t proto = 1;
  uint64_t epoch = 0;
  uint8_t qual = 0;
  bool setBoot = false;

  bool logWasActive = true;
  bool logSuspended = false;
  bool logResumed = false;
  std::string openPath; // held open -> ERR_BUSY

  std::vector<uint8_t> rx; // bytes Prime "sent" us, drained by readByte()
  size_t rxPos = 0;
  std::vector<uint8_t> tx; // bytes Light wrote back
  std::vector<std::string> diags;

  // --- helpers used by the tests ---
  void give(const uint8_t *d, size_t n) { rx.insert(rx.end(), d, d + n); }

  FFile *find(const char *path) {
    for (auto &f : files)
      if (f.path == path) return &f;
    return nullptr;
  }

  bool has(const char *path) { return find(path) != nullptr; }

  // --- transport ---
  int readByte() override {
    if (rxPos >= rx.size()) return -1;
    return rx[rxPos++];
  }
  void write(const uint8_t *data, size_t n) override {
    tx.insert(tx.end(), data, data + n);
  }

  // --- identity ---
  uint16_t protoVersion() override { return proto; }
  const char *product() override { return "Scottina Light"; }
  const char *fwVersion() override { return "v1-foundation"; }

  // --- storage ---
  bool sdMounted() override { return sd; }
  bool exists(const char *path) override { return sd && has(path); }

  bool remove(const char *path) override {
    if (!sd) return false;
    for (size_t i = 0; i < files.size(); ++i) {
      if (files[i].path == path) {
        files.erase(files.begin() + i);
        return true;
      }
    }
    return false;
  }

  bool rename(const char *from, const char *to) override {
    if (!sd) return false;
    FFile *f = find(from);
    if (!f) return false;
    remove(to);
    f->path = to;
    return true;
  }

  uint32_t fileSize(const char *path) override {
    FFile *f = sd ? find(path) : nullptr;
    return f ? (uint32_t)f->data.size() : 0;
  }

  int listDir(const char *dir, uint16_t startIndex, dock::DirEnt *out, int maxOut,
              bool *more) override {
    *more = false;
    if (!sd) return -1;

    // Lexicographic, matching the fake Light the vectors were built against.
    // Real firmware order is unspecified -- but it MUST be stable within a dock
    // session, or index-based paging would skip entries as it walked.
    std::vector<FFile *> hits;
    const size_t dlen = strlen(dir);
    for (auto &f : files) {
      if (f.path.compare(0, dlen, dir) != 0) continue;
      if (f.path.find('/', dlen) != std::string::npos) continue; // not in this dir
      if (f.path == openPath) continue;                          // the active log is never listed
      if (f.path.size() > 8 && f.path.compare(f.path.size() - 8, 8, ".partial") == 0)
        continue; // staging files are not real files yet
      hits.push_back(&f);
    }
    std::sort(hits.begin(), hits.end(),
              [](FFile *a, FFile *b) { return a->path < b->path; });

    int filled = 0;
    for (size_t i = startIndex; i < hits.size(); ++i) {
      if (filled >= maxOut) { *more = true; break; }
      snprintf(out[filled].name, sizeof(out[filled].name), "%s",
               hits[i]->path.c_str() + dlen);
      out[filled].size = (uint32_t)hits[i]->data.size();
      filled++;
    }
    return filled;
  }

  int readAt(const char *path, uint32_t offset, uint8_t *out, uint16_t n) override {
    if (!sd) return -1;
    FFile *f = find(path);
    if (!f) return -1;
    if (offset >= f->data.size()) return 0;
    const uint32_t remain = (uint32_t)f->data.size() - offset;
    const uint16_t take = (uint16_t)((remain < n) ? remain : n);
    memcpy(out, f->data.data() + offset, take);
    return take;
  }

  int appendTo(const char *path, const uint8_t *data, uint16_t n) override {
    if (!sd) return -1;
    FFile *f = find(path);
    if (!f) {
      files.push_back({path, {}});
      f = &files.back();
    }
    f->data.insert(f->data.end(), data, data + n);
    return n;
  }

  bool truncate(const char *path) override {
    if (!sd) return false;
    FFile *f = find(path);
    if (f) {
      f->data.clear();
      return true;
    }
    files.push_back({path, {}});
    return true;
  }

  // --- logger ---
  void suspendLogging() override {
    // Suspending nothing is not a suspension -- mirrors the real logger.
    if (!logWasActive) return;
    logSuspended = true;
  }
  void resumeLogging() override {
    if (logSuspended) logResumed = true;
    logSuspended = false;
  }
  bool loggingWasActive() override { return logWasActive; }
  bool loggingSuspended() override { return logSuspended; }
  const char *activeLogPath() override {
    return openPath.empty() ? nullptr : openPath.c_str();
  }

  // --- clock ---
  void setClock(uint64_t e, uint8_t q) override {
    epoch = e;
    qual = q;
    setBoot = true;
  }
  uint64_t clockEpoch() override { return epoch; }
  uint8_t clockQuality() override { return qual; }
  bool clockSetThisBoot() override { return setBoot; }

  void diag(const char *msg) override { diags.push_back(msg); }
};

#endif // SL_FAKE_PLATFORM_H
