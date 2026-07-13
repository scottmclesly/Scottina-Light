// SHA-256 -- compact public-domain implementation (FIPS 180-4).
//
// Vendored because the SAMD51 has NO crypto accelerator (that is the SAML
// family), so this is plain CPU work, and both COMMIT and DELETE depend on it.
//
// Cost, measured on paper and worth remembering: hashing is ~2 s of CPU for an
// 8 MB log on a 120 MHz M4F, but READING that log over the 8 MHz SPI-SD bus is
// 16-27 s. The read dominates. This is why GET hashes incrementally as it
// streams (one pass, no second read) and why DELETE gets a 60 s timeout in
// DOCK-PROTOCOL.md §5.

#ifndef SL_SHA256_H
#define SL_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]); // out is 32 raw bytes

#ifdef __cplusplus
}
#endif

#endif // SL_SHA256_H
