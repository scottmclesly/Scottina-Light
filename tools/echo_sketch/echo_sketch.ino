// Phase-0 bench instrument: CDC throughput echo.
//
// Flash this to Light, then drive it from Prime (NOT from a serial monitor --
// a monitor's line discipline will dominate the measurement). Prime writes N
// bytes and reads them back; the round-trip rate sets the progress-bar math and
// tells us whether an 8 MB log pull is bandwidth-bound or round-trip-bound.
//
// The nominal baud is ignored -- USB CDC runs at native full speed.
//
//   pio run -t upload   (with this as the only .ino/.cpp in a scratch project)
//
// Prime side, roughly:
//   import serial, time
//   p = serial.Serial(port, 115200, timeout=5)
//   blob = bytes(range(256)) * 256          # 64 KiB
//   t = time.perf_counter()
//   for _ in range(16):                      # 1 MiB total
//       p.write(blob); got = p.read(len(blob))
//       assert got == blob
//   print(1024*1024 / (time.perf_counter() - t) / 1e6, "MB/s round-trip")
//
// Also worth measuring separately, because it is what actually bounds a chunked
// GET: the per-frame round-trip latency. Echo a single 1024-byte buffer 1000
// times and divide -- 8 MB at max_payload=1024 is 8192 sequential round trips,
// and if each costs ~1 ms of USB latency that is ~8 s of pure stall before any
// SD reading happens. That number is the argument for a larger max_payload
// (which HELLO negotiates, so raising it costs no protocol change).

#include <Arduino.h>

static uint8_t buf[1024];

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ; // wait for the host to open the port
  }
}

void loop() {
  const int n = Serial.available();
  if (n <= 0) return;
  const size_t got = Serial.readBytes(buf, (size_t)min(n, (int)sizeof(buf)));
  Serial.write(buf, got);
}
