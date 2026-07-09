#include "scottina_light.h"

namespace i2cbus {

// --- identity checks -------------------------------------------------------
// Each reads a device's WHO_AM_I-equivalent and compares against the datasheet
// value. These are what let an address collision resolve without asking.

static bool verifyLis3dh(TwoWire &w, uint8_t a) {
  uint8_t v;
  return readReg(w, a, 0x0F, v) && v == 0x33;
}
static bool verifyMpu6050(TwoWire &w, uint8_t a) {
  uint8_t v;
  return readReg(w, a, 0x75, v) && v == 0x68;
}
static bool verifyBme280(TwoWire &w, uint8_t a) {
  uint8_t v;
  return readReg(w, a, 0xD0, v) && v == 0x60;
}
static bool verifyBmp280(TwoWire &w, uint8_t a) {
  uint8_t v;
  return readReg(w, a, 0xD0, v) && v == 0x58;
}
static bool verifyBmp180(TwoWire &w, uint8_t a) {
  uint8_t v;
  return readReg(w, a, 0xD0, v) && v == 0x55;
}

// Address map. Deliberately includes the real-world collisions -- 0x68 is both
// an IMU and two different RTCs, 0x40 is three unrelated parts -- because that
// is exactly what the pick-list in §0 exists to handle.
const KnownDev KNOWN[] = {
    {0x18, "LIS3DHTR accel", Cap::Accel, verifyLis3dh},
    {0x19, "LIS3DHTR accel (alt)", Cap::Accel, verifyLis3dh},
    {0x23, "BH1750 light", Cap::Light, nullptr},
    {0x29, "VL53L0X range", Cap::Distance, nullptr},
    {0x29, "TSL2561 light", Cap::Light, nullptr},
    {0x39, "APDS9960 gesture", Cap::Light, nullptr},
    {0x40, "INA219 current", Cap::Current, nullptr},
    {0x40, "HTU21D temp/hum", Cap::TempHum, nullptr},
    {0x40, "Si7021 temp/hum", Cap::TempHum, nullptr},
    {0x44, "SHT3x temp/hum", Cap::TempHum, nullptr},
    {0x45, "SHT3x temp/hum (alt)", Cap::TempHum, nullptr},
    {0x48, "ADS1115 ADC", Cap::Adc, nullptr},
    {0x48, "TMP102 temp", Cap::TempHum, nullptr},
    {0x48, "LM75 temp", Cap::TempHum, nullptr},
    {0x53, "ADXL345 accel", Cap::Accel, nullptr},
    {0x68, "MPU6050 6-DoF", Cap::Imu6, verifyMpu6050},
    {0x68, "DS3231 RTC", Cap::RtcClock, nullptr},
    {0x68, "DS1307 RTC", Cap::RtcClock, nullptr},
    {0x69, "MPU6050 6-DoF (alt)", Cap::Imu6, verifyMpu6050},
    {0x76, "BME280 baro", Cap::Baro, verifyBme280},
    {0x76, "BMP280 baro", Cap::Baro, verifyBmp280},
    {0x77, "BME280 baro (alt)", Cap::Baro, verifyBme280},
    {0x77, "BMP280 baro (alt)", Cap::Baro, verifyBmp280},
    {0x77, "BMP180 baro", Cap::Baro, verifyBmp180},
};
const uint8_t KNOWN_N = sizeof(KNOWN) / sizeof(KNOWN[0]);

const char *capName(Cap c) {
  switch (c) {
  case Cap::Accel: return "accel";
  case Cap::TempHum: return "temp/hum";
  case Cap::Baro: return "baro";
  case Cap::Light: return "light";
  case Cap::Distance: return "range";
  case Cap::RtcClock: return "rtc";
  case Cap::Adc: return "adc";
  case Cap::Imu6: return "imu6";
  case Cap::Current: return "current";
  default: return "unknown";
  }
}

void beginBus(TwoWire &w) {
  w.begin();
  // Burn one transaction. See the note in scottina_light.h -- the first probe after a
  // cold begin() on an unpulled bus ACKs whatever address it is given. 0x7F is
  // in the reserved 10-bit-addressing block, so nothing real can answer it and
  // the result is discarded regardless.
  w.beginTransmission(0x7F);
  (void)w.endTransmission();
}

bool probe(TwoWire &w, uint8_t addr) {
  w.beginTransmission(addr);
  return w.endTransmission() == 0;
}

bool readReg(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t &val) {
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom((int)addr, 1) != 1) return false;
  val = (uint8_t)w.read();
  return true;
}

uint8_t scan(TwoWire &w, uint8_t *out, uint8_t maxOut) {
  uint8_t n = 0;
  for (uint8_t a = 0x08; a < 0x78 && n < maxOut; ++a) {
    if (!probe(w, a)) continue;
    if (!probe(w, a)) continue; // must ACK twice
    out[n++] = a;
  }
  return n;
}

uint8_t candidatesFor(uint8_t addr, const KnownDev **out, uint8_t maxOut) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < KNOWN_N && n < maxOut; ++i) {
    if (KNOWN[i].addr == addr) out[n++] = &KNOWN[i];
  }
  return n;
}

const KnownDev *autoResolve(TwoWire &w, uint8_t addr) {
  const KnownDev *cand[8];
  const uint8_t n = candidatesFor(addr, cand, 8);
  if (n == 0) return nullptr;
  if (n == 1) return cand[0];

  const KnownDev *hit = nullptr;
  uint8_t hits = 0;
  for (uint8_t i = 0; i < n; ++i) {
    if (cand[i]->verify && cand[i]->verify(w, addr)) {
      hit = cand[i];
      hits++;
    }
  }
  return hits == 1 ? hit : nullptr; // 0 or >1 => the user decides
}

} // namespace i2cbus

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

namespace inventory {

Detected devs[MAX];
uint8_t count = 0;

static void addBus(TwoWire &bus, const char *busName) {
  uint8_t addrs[i2cbus::MAX_DEV];
  const uint8_t n = i2cbus::scan(bus, addrs, i2cbus::MAX_DEV);

  for (uint8_t i = 0; i < n && count < MAX; ++i) {
    Detected &d = devs[count];
    d.addr = addrs[i];
    d.bus = &bus;
    d.busName = busName;
    d.ambiguous = false;
    d.name = "unknown";
    d.cap = i2cbus::Cap::Unknown;

    const i2cbus::KnownDev *k = i2cbus::autoResolve(bus, addrs[i]);
    if (k) {
      d.name = k->name;
      d.cap = k->cap;
    } else {
      const i2cbus::KnownDev *cand[8];
      const uint8_t cn = i2cbus::candidatesFor(addrs[i], cand, 8);
      if (cn > 1) {
        // A previously persisted Tier-2 pick settles it without re-asking.
        char saved[40];
        if (config::loadChoice(addrs[i], saved, sizeof(saved))) {
          for (uint8_t c = 0; c < cn; ++c) {
            if (strcmp(cand[c]->name, saved) == 0) {
              d.name = cand[c]->name;
              d.cap = cand[c]->cap;
              break;
            }
          }
        }
        if (d.cap == i2cbus::Cap::Unknown) {
          d.ambiguous = true;
          d.name = cand[0]->name; // provisional; screen offers the pick-list
        }
      }
    }
    count++;
  }
}

void rescan() {
  count = 0;
  addBus(Wire, "grove");
  addBus(Wire1, "internal");
}

bool hasCap(i2cbus::Cap c) {
  for (uint8_t i = 0; i < count; ++i) {
    if (devs[i].cap == c && !devs[i].ambiguous) return true;
  }
  return false;
}

Detected *findByAddr(uint8_t addr) {
  for (uint8_t i = 0; i < count; ++i) {
    if (devs[i].addr == addr) return &devs[i];
  }
  return nullptr;
}

} // namespace inventory
