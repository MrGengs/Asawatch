/*
 * max30102_testing -- tes sensor MAX30100 / MAX30102 di ESP32-C3
 *
 * Tanpa library eksternal (hanya Wire). Chip dikenali otomatis dari register
 * PART_ID (0xFF): 0x15 = MAX30102, 0x11 = MAX30100. Alamat I2C keduanya 0x57.
 *
 * WIRING (modul breakout MAX30100/MAX30102 -> ESP32-C3)
 *   +-------------------+--------------------+------------------------------+
 *   | Pin modul sensor  | Pin ESP32-C3       | Catatan                      |
 *   +-------------------+--------------------+------------------------------+
 *   | VIN / VCC         | 3V3                | JANGAN 5V (kecuali modul     |
 *   |                   |                    | punya regulator + level      |
 *   |                   |                    | shifter, cek dulu)           |
 *   | GND               | GND                |                              |
 *   | SDA               | GPIO4 (SDA_PIN)    | pull-up 4.7k ke 3V3 sudah    |
 *   | SCL               | GPIO5 (SCL_PIN)    | ada di kebanyakan modul      |
 *   | INT               | GPIO3 (opsional)   | tidak dipakai sketch ini     |
 *   | IRD / RD (LED)    | -                  | jangan disambung             |
 *   +-------------------+--------------------+------------------------------+
 *   GPIO4/5 dipilih karena bukan pin strapping (GPIO2/8/9 sebaiknya dihindari).
 *   Ganti SDA_PIN / SCL_PIN di bawah kalau wiring berbeda.
 *
 * Arduino IDE: board "ESP32C3 Dev Module", USB CDC On Boot = Enabled.
 * Serial 115200. Letakkan jari (tanpa menekan keras) di atas sensor.
 *   PLOT_MODE 1 -> keluaran "ir,red" untuk Serial Plotter
 *   PLOT_MODE 0 -> status teks tiap 500 ms
 */

#include <Arduino.h>
#include <Wire.h>

#define SDA_PIN   4
#define SCL_PIN   5
#define PLOT_MODE 0

static const uint8_t ADDR = 0x57;

enum Chip { CHIP_NONE, CHIP_30100, CHIP_30102 };
static Chip chip = CHIP_NONE;

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ADDR, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static uint8_t rd8(uint8_t reg) {
  uint8_t v = 0;
  rd(reg, &v, 1);
  return v;
}

/* ---------- init per chip ---------- */

static void init30102() {
  wr(0x09, 0x40);              // reset
  delay(100);
  wr(0x09, 0x03);              // mode SpO2 (Red + IR)
  wr(0x08, 0x5F);              // avg 4, rollover aktif, almost-full 15
  wr(0x0A, 0x2F);              // ADC 4096 nA, 400 sps (/4 avg = 100 Hz), pw 411 us (18 bit)
  wr(0x0C, 0x30);              // LED Red ~10 mA
  wr(0x0D, 0x30);              // LED IR  ~10 mA
  wr(0x04, 0); wr(0x05, 0); wr(0x06, 0);   // kosongkan FIFO
}

static void init30100() {
  wr(0x06, 0x40);              // reset
  delay(100);
  wr(0x06, 0x03);              // mode SpO2 (Red + IR)
  wr(0x07, 0x47);              // hi-res, 100 sps, pw 1600 us (16 bit)
  wr(0x09, 0x77);              // LED Red 24 mA (nibble atas) | IR 24 mA (nibble bawah)
  wr(0x02, 0); wr(0x03, 0); wr(0x04, 0);   // kosongkan FIFO
}

/* ---------- baca FIFO ---------- */

// Mengisi satu sampel terbaru; return jumlah sampel yang dibaca dari FIFO.
static int readFifo(uint32_t &ir, uint32_t &red) {
  uint8_t b[6];
  int got = 0;

  if (chip == CHIP_30102) {
    int n = (rd8(0x04) - rd8(0x06)) & 0x1F;
    if (n == 0 && rd8(0x05) != 0) n = 32;      // overflow -> FIFO penuh
    while (n-- > 0) {
      if (!rd(0x07, b, 6)) break;
      red = (((uint32_t)b[0] << 16) | (b[1] << 8) | b[2]) & 0x3FFFF;
      ir  = (((uint32_t)b[3] << 16) | (b[4] << 8) | b[5]) & 0x3FFFF;
      got++;
    }
  } else if (chip == CHIP_30100) {
    int n = (rd8(0x02) - rd8(0x04)) & 0x0F;
    if (n == 0 && rd8(0x03) != 0) n = 16;
    while (n-- > 0) {
      if (!rd(0x05, b, 4)) break;              // urutan: IR(2 byte), Red(2 byte)
      ir  = ((uint32_t)b[0] << 8) | b[1];
      red = ((uint32_t)b[2] << 8) | b[3];
      got++;
    }
  }
  return got;
}

/* ---------- deteksi BPM sederhana ---------- */

static float dc = 0, amp = 0;
static float sm[3] = {0, 0, 0};                // smoothed AC: [0]=terlama
static float win[4] = {0};
static uint8_t winIdx = 0;
static uint32_t lastPeakMs = 0, sampleCount = 0;
static float bpmHist[4] = {0};
static uint8_t bpmN = 0, bpmIdx = 0;
static float bpm = 0;

static void resetBpm() {
  dc = amp = 0; sampleCount = 0; lastPeakMs = 0;
  bpmN = bpmIdx = 0; bpm = 0;
  for (auto &v : sm) v = 0;
  for (auto &v : win) v = 0;
}

static void feedBpm(uint32_t ir) {
  if (sampleCount == 0) dc = ir;
  dc += 0.05f * (ir - dc);                     // EMA = komponen DC
  float ac = ir - dc;

  win[winIdx++ & 3] = ac;                      // moving average 4 sampel
  float s = (win[0] + win[1] + win[2] + win[3]) / 4.0f;
  sm[0] = sm[1]; sm[1] = sm[2]; sm[2] = s;

  amp += 0.02f * (fabsf(s) - amp);
  sampleCount++;
  if (sampleCount < 150) return;               // tunggu filter settle (~1.5 s)

  // puncak lokal di sm[1], di atas ambang relatif terhadap amplitudo
  if (sm[1] > sm[0] && sm[1] >= sm[2] && sm[1] > 0.5f * amp) {
    uint32_t now = millis();
    if (lastPeakMs && now - lastPeakMs > 300) {      // refractory 300 ms (maks 200 bpm)
      float inst = 60000.0f / (now - lastPeakMs);
      if (inst >= 40 && inst <= 180) {
        bpmHist[bpmIdx++ & 3] = inst;
        if (bpmN < 4) bpmN++;
        float sum = 0;
        for (uint8_t i = 0; i < bpmN; i++) sum += bpmHist[i];
        bpm = sum / bpmN;
      }
      lastPeakMs = now;
    } else if (!lastPeakMs) {
      lastPeakMs = now;
    }
  }
}

/* ---------- main ---------- */

static uint32_t lastIr = 0, lastRed = 0, lastPrint = 0;
static bool finger = false;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[max30102_testing] ESP32-C3");

  Wire.begin(SDA_PIN, SCL_PIN, 400000);

  Wire.beginTransmission(ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.printf("[err] tidak ada perangkat di 0x57 (SDA=GPIO%d SCL=GPIO%d). Cek wiring/VCC.\n",
                  SDA_PIN, SCL_PIN);
    return;
  }

  uint8_t id = rd8(0xFF);
  if (id == 0x15)      { chip = CHIP_30102; init30102(); Serial.println("[ok] MAX30102 (PART_ID 0x15)"); }
  else if (id == 0x11) { chip = CHIP_30100; init30100(); Serial.println("[ok] MAX30100 (PART_ID 0x11)"); }
  else Serial.printf("[err] PART_ID tidak dikenal: 0x%02X (bukan MAX30100/30102?)\n", id);
}

void loop() {
  if (chip == CHIP_NONE) { delay(1000); return; }

  uint32_t ir, red;
  int n = readFifo(ir, red);
  if (n > 0) {
    lastIr = ir; lastRed = red;
    uint32_t thr = (chip == CHIP_30102) ? 15000 : 8000;
    bool now = ir > thr;
    if (now != finger) { finger = now; resetBpm(); }
    if (finger) feedBpm(ir);
#if PLOT_MODE
    Serial.printf("%lu,%lu\n", (unsigned long)ir, (unsigned long)red);
#endif
  }

#if !PLOT_MODE
  if (millis() - lastPrint >= 500) {
    lastPrint = millis();
    if (!finger) {
      Serial.printf("ir=%lu red=%lu  -> letakkan jari di sensor\n",
                    (unsigned long)lastIr, (unsigned long)lastRed);
    } else if (bpm > 0) {
      Serial.printf("ir=%lu red=%lu  >>> BPM: %.0f <<<\n",
                    (unsigned long)lastIr, (unsigned long)lastRed, bpm);
    } else {
      Serial.printf("ir=%lu red=%lu  BPM: ... (mengukur, tahan jari diam ~5 dtk)\n",
                    (unsigned long)lastIr, (unsigned long)lastRed);
    }
  }
#endif
  delay(5);
}
