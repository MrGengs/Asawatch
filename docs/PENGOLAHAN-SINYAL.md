# AsaWatch — Pengolahan Sinyal PPG, Filtering, dan Kalibrasi

Dokumen ini menjelaskan **dari mana angka detak jantung, SpO2, glukosa, dan
tekanan darah di paket `Sampel` berasal**: tahap demi tahap dari sensor sampai
byte yang dikirim ke aplikasi, apa yang dibuang di tiap tahap dan kenapa, serta
**status kalibrasi setiap metrik apa adanya**.

Acuannya adalah `ppg.{h,cpp}` dan `aw_jam.cpp` di firmware, ditambah sisi
aplikasi di `Flutter/asawatch/lib/models/sesi_makan.dart` (`Kalibrasi`,
`PutaranKalibrasi`) dan `kalibrasi_tekanan_darah_page.dart`.

**Ada dua generasi DSP di repo ini** sejak commit `800a2c9` (2026-09-11):

| Folder | Generasi | Keterangan |
|---|---|---|
| `no_touch/` | **Dasar** | Seperti yang sudah dikapalkan; sengaja tidak disentuh |
| `no_touch-callibration/` | **Kalibrasi** | Salinan penuh `no_touch/` + *calibration pass* (§2.10) |
| `touchscreen/` | **Kalibrasi** | Calibration pass yang sama diterapkan langsung |

§1–§2.9 menjelaskan pipeline dasar yang sama di ketiganya; §2.10 menjelaskan
apa yang ditambahkan generasi kalibrasi dan di mana ia menyisip. Data lapangan
yang mendasarinya ada di `Data_Kalibrasi_Asawatch_Gabungan.md` di akar repo.

Gambaran sistem secara keseluruhan (siapa mengirim apa ke siapa) ada di
`web/asawatch_web/docs/CARA-KERJA.md`; dokumen ini hanya membahas isi kotak
"sensor → angka".

---

## 0. Peringatan yang harus dibaca lebih dulu

| Metrik | Dasar | Status kalibrasi | Boleh dipercaya untuk |
|---|---|---|---|
| Detak jantung (BPM) | Deteksi puncak pada sinyal IR — metode standar | Tidak butuh kalibrasi; akurasinya soal kualitas kontak | Tren dan angka kasar |
| SpO2 | Formula empiris Maxim AN6409 atas rasio R | Offset situs wrist **+16,8 %** dari **satu** perbandingan (2026-07-29) | Tren; angka absolutnya provisional |
| Glukosa | **Tidak ada dasar fisiologis tervalidasi.** Regresi linear placeholder atas (PI, HR, R) | Dasar: `G0` dari 1 titik (GCHb). Kalibrasi: `G0` dari **21 titik / 12 subjek** vs Autocheck — tetapi hanya bias, simpangan bakunya 16 mg/dL. `G1`–`G3` **belum pernah** disentuh data nyata di keduanya | Tidak untuk keputusan medis apa pun. Nilai riset/eksplorasi |
| Tekanan darah | Sama: regresi placeholder atas (PI, HR, R). Perangkat medis memakai *pulse transit time* dengan dua sensor; di sini hanya satu PPG | **Nol** titik data untuk koefisien; ada offset per-orang dari manset (§5) | Sama seperti glukosa |

Bit `AW_KEMAMPUAN` untuk glukosa dan tensi tetap menyala karena artinya "jam
mengirim metrik ini", bukan "angkanya layak klinis". Peringatan lengkap ada di
`ppg.h`.

---

## 1. Pipeline dari sensor ke paket

```
MAX30105/30102, LED Red + IR, 100 Hz, SAMPLE_AVERAGE 8  →  FIFO keluar 12,5 sampel/dtk
        │
        ▼  process_sample(red, ir)
┌─ 1. Gerbang kontak kulit   ir ≥ 30000, debounce 250 ms; kontak baru → reset semua
│
├─ 2. Pemisahan DC / AC      DC = EMA(α 0,95, τ ≈ 1,6 dtk); AC = mentah − DC
│                             tunggu settle 3 dtk sebelum apa pun dihitung
│
├─ 3. Deteksi detak (IR)     detect_beat(): smoothing → band-pass kasar → threshold adaptif
│                             → IBI → BPM = rata-rata 4 IBI
│
├─ 4. Jendela 1 dtk          accumulate_spo2(): RMS AC & rata-rata DC per kanal
│                             → R = (ACrms_R/DC_R) / (ACrms_IR/DC_IR),  PI = ACrms_IR/DC_IR × 100
│                             → buang jendela di luar rentang plausible
│
├─ 5. Estimasi per jendela   SpO2 = AN6409(R) + 16,8 ; glukosa = f(PI,HR,R) ; sis/dia = g(PI,HR,R)
│                             [kalibrasi] R & PI lewat median-of-3 dulu sebelum glukosa/tensi; SpO2 tetap R mentah
│                             [kalibrasi] AGC: arus LED per kanal disesuaikan tiap ≥4 dtk bila DC terlalu tinggi/rendah
│
├─ 6. Penghalusan            push_smoothed(): rata-rata bergerak 5 jendela (yang tampil di layar)
│                             update_session_stats(): min/maks/rata seluruh jendela STABIL (yang dikirim)
│
▼  aw_jam.cpp  ukur_putar() / ukur_selesai()
┌─ 7. Gerbang selesai        keempat metrik pernah sah + ≥10 detak + ≥10 dtk  (bukan "waktu habis")
├─ 8. Nilai final            BPM/SpO2/sis/dia = rata-rata sesi; glukosa = nilai terkini yang dihaluskan
├─ 9. Kalibrasi tensi        sis += offset_sis, dia += offset_dia (dari NVS, bila ada), klem 1..255
│   [kalibrasi] glukosa     gula += offset_glu (NVS, diset lewat konsol serial "glu_offset N"), klem 1..65535
└─ 10. Paket Sampel          uint16 gula (mg/dL), uint8 bpm, sis, dia, spo2 — **0 = gagal diukur**
```

Semua tahap 1–6 berjalan di konteks `loop()` lewat `ppg_update()`, yang hanya
mengambil apa yang sudah ada di FIFO (non-blocking; `getRed()/getIR()` dari
library sengaja tidak dipakai karena memblokir sampai 250 ms).

---

## 2. Tahap demi tahap

### 2.1 Konfigurasi sensor

| Parameter | Nilai | Alasan |
|---|---|---|
| `LED_BRIGHTNESS` | `0xFF` (arus penuh) | Sinyal di pergelangan (arteri radial) jauh lebih lemah daripada ujung jari |
| `SAMPLE_AVERAGE` | 8 | Meredam noise; konsekuensinya FIFO mengeluarkan **12,5 sampel/dtk**, bukan 100 |
| `LED_MODE` | 2 (Red + IR) | Kanal hijau tidak dipakai — unit uji tidak memancarkan hijau (kemungkinan MAX30102) |
| `SAMPLE_RATE` / `PULSE_WIDTH` / `ADC_RANGE` | 100 Hz / 411 µs / 16384 | ADC 18-bit; range besar supaya arus penuh tidak saturasi |
| Bus I2C | 100 kHz | Belum pernah diuji 400 kHz untuk kombinasi RTC + PPG di board ini |

LED **mati secara bawaan** dan hanya menyala selama pengukuran
(`ppg_set_enabled`): dua LED pada arus penuh menyedot puluhan mA walau tidak
ada kulit menempel.

### 2.2 Gerbang kontak kulit

- Kontak = `ir ≥ IR_PRESENCE_THRESHOLD (30000)`.
- **Debounce 250 ms** (`CONTACT_DEBOUNCE_MS`): tanpa ini, kontak yang goyang
  sesaat dianggap "terlepas" dan me-reset seluruh detektor, membuang progres
  settle.
- Setiap **kontak baru** me-reset filter DC, detektor detak, statistik sesi,
  dan hasil tertahan. Ini wajib: transien saat DC "mengejar" baseline baru
  akan termakan `peakEnvelope` (yang hanya naik, tidak turun sendiri) dan
  membuat detak asli tidak pernah lolos threshold lagi.
- Sensor diangkat → hasil terakhir **ditahan** (`s_hold`) untuk layar; DSP
  tetap di-reset.

Diagnostik `s_last_ir`, `s_samples`, `s_polls` membedakan "sensor tidak
dicatu", "ambang terlalu tinggi", dan "memang tidak ada kulit" — chip bisa
ACK di I2C hanya dari pull-up sementara LED-nya gelap.

### 2.3 Pemisahan DC / AC

```
dc  = dc × 0,95 + mentah × 0,05          (per kanal, di-seed dari sampel pertama)
ac  = mentah − dc
```

Konstanta waktu EMA = 1/(1−0,95) = 20 sampel ≈ **1,6 dtk** pada 12,5 sampel/dtk.
`DC_SETTLE_MS = 3000` ≈ dua konstanta waktu (sisa transien < 15 %). Selama
settle, `ac` didominasi transien, jadi **tidak** diumpankan ke detektor detak
dan tidak ada estimasi yang dihitung; layar menampilkan `--`.

Sejarahnya: settle dulu 8 dtk dan jendela SpO2 "100 sampel ≈ 1 dtk" — padahal
dengan `SAMPLE_AVERAGE 8` itu **8 dtk**, sehingga angka pertama baru muncul di
detik ke-16. Keduanya sudah dihitung ulang; jangan dikembalikan ke jumlah
sampel.

### 2.4 Deteksi detak (`detect_beat`, kanal IR)

```
acSmooth  = acSmoothPrev × 0,7 + ac × 0,3           smoothing ringan, anti noise 1 sampel
slowAC    = slowAC × 0,97 + acSmooth × 0,03          EMA cutoff ≈ 0,5 Hz
cardiac   = acSmooth − slowAC                        ← band-pass kasar: buang napas/gerakan (0,2–0,4 Hz)
threshold = peakEnvelope × 0,5                       dari envelope SEBELUM sampel ini
rising    = cardiac > threshold && cardiacPrev ≤ threshold
```

- Deteksi = *rising crossing* dengan **refractory `MIN_IBI_MS` 300 ms**
  (maks 200 bpm).
- IBI pertama dibuang, IBI > 2000 ms (< 30 bpm) dibuang.
- `BPM = 60000 / rata-rata 4 IBI terakhir`.
- `peakEnvelope = envelope × 0,98 + |cardiac| × 0,02` — diperbarui **setelah**
  threshold dihitung; kalau tidak, threshold "mengejar" puncak yang sedang
  dievaluasi dan crossing gagal.
- Dua pencacah: `stableBeatCount` berhenti di `MIN_STABLE_BEATS = 3`
  (gerbang biner "sudah konsisten"), `beatCount` terus naik (dipakai `aw_jam`
  untuk syarat ≥10 detak).

Kenapa high-pass DC saja tidak cukup: modulasi napas/gerakan sering jauh lebih
besar amplitudonya daripada detak; threshold adaptif akan "terkunci" ke ayunan
napas.

### 2.5 Jendela 1 detik dan fitur R, PI

`accumulate_spo2()` mengumpulkan per jendela **`SPO2_WINDOW_MS = 1000`**
(minimal `SPO2_WINDOW_MIN = 8` sampel):

```
ACrms_R, ACrms_IR   = sqrt(Σac² / n)
DC_R, DC_IR         = Σdc / n
R  = (ACrms_R / DC_R) / (ACrms_IR / DC_IR)
PI = (ACrms_IR / DC_IR) × 100        [%]
```

Gerbang menghitung: settle selesai **dan** `bpmValid`. Yang kedua bukan sisa
lama — model glukosa/tensi memakai `HR − 70`; dengan BPM masih 0 sukunya
menjadi `G2 × (−70)` dan hasilnya bukan "sementara" melainkan salah.

**Jendela dibuang** (tidak dihitung, tidak dihaluskan, tidak masuk statistik)
bila di luar rentang plausible:

| Fitur | Rentang | Alasan |
|---|---|---|
| R | 0,3 – 1,15 | AN6409 hanya valid ~0,4–1,0 (SpO2 100 % → 85 %); 1,15 = toleransi bawah (SpO2 ~70 %) |
| PI | 0,02 – 15 % | > 15 % hampir pasti artefak gerak |
| SpO2 hasil | ≥ 70 % | Manusia hidup selalu ≥ 70 % |

Satu jendela ≈ 1 detak, jadi R per jendela memang berisik. Kestabilan sengaja
diambil dari dua lapis di atasnya (§2.7), bukan dari jendela panjang — supaya
angka di layar bergerak sambil diukur.

### 2.6 Estimasi per jendela

**SpO2** (Maxim AN6409, empiris untuk geometri jari):

```
spo2 = −45,060·R² + 30,354·R + 94,845
spo2 += SPO2_WRIST_OFFSET (16,8)      ← lihat §4.2
spo2 = min(spo2, 100)
```

**Glukosa** (`estimate_glucose`) — placeholder:

```
glukosa_mgdl = G0 + G1·PI + G2·(HR − 70) + G3·R
G0 = 82,1 (dasar) | 88,2 (kalibrasi)    G1 = −15,0   G2 = 0,3   G3 = 20,0
```

**Tekanan darah** (`estimate_bp`) — placeholder, titik tolak 120/80:

```
sistol  = SBP0 + SBP1·(HR − 70) + SBP2·PI + SBP3·R     SBP = 120,0 / 0,3 / −3,0 / 15,0
diastol = DBP0 + DBP1·(HR − 70) + DBP2·PI + DBP3·R     DBP =  80,0 / 0,2 / −2,0 / 10,0
diastol = min(diastol, sistol − 10)                    ← jaring pengaman "140/150", bukan fisiologi
```

### 2.7 Penghalusan dan dua gerbang

- `push_smoothed()`: rata-rata bergerak **`SMOOTH_N = 5` jendela** untuk
  SpO2, glukosa, sis, dia. Ini yang **tampil di layar**.
- `update_session_stats()`: min/maks/jumlah seluruh jendela — **hanya jendela
  stabil** (`stableBeatCount ≥ 3`). Ini sumber angka yang **dikirim**.

| Bendera | Arti | Dipakai oleh |
|---|---|---|
| `readingsAwal` | jendela terakhir menghasilkan angka masuk akal | UI (ditampilkan dengan warna beda) |
| `readingsStable` | angka itu lahir setelah ≥3 detak konsisten | `aw_jam` (boleh dikirim) |

Dulu keduanya satu bendera, dan layar ikut menunggu syarat yang sebenarnya
milik protokol.

### 2.8 Selesai karena data cukup, bukan karena waktu habis (`aw_jam.cpp`)

`ukur_putar()` membaca `ppg_get()` tiap putaran dan **mengunci nilai sah
pertama** tiap metrik ke akumulator (`s_acc_*`) — metrik matang pada waktu
berbeda (BPM dulu, glukosa & tensi paling akhir), dan menunggu keempatnya
matang bersamaan sering berakhir tangan kosong.

Selesai bila **semua** terpenuhi:

| Syarat | Nilai |
|---|---|
| keempat metrik pernah sah | `s_acc_bpm && s_acc_spo2 && s_acc_gula && s_acc_sis` |
| detak cukup | `beatCount ≥ UKUR_MIN_DETAK (10)` |
| lama cukup | `≥ UKUR_MIN_MS (10 dtk)` — lantai supaya nadi cepat tidak "lulus" dalam 5 detik |

Lalu nilai final **diambil ulang**: BPM, SpO2, sis, dia dari **rata-rata
sesi** (`*_avg`), glukosa dari nilai terkini yang sudah dihaluskan (tidak ada
rata-rata sesi untuk glukosa). Itulah alasan menunggu 10 detak: kalau yang
dikirim toh jendela pertama, menunggu tidak memperbaiki apa pun.

Dua penjaga **daya** (bukan kesabaran): 90 dtk tanpa kulit menempel
(`UKUR_TANPA_KONTAK_MS`) dan batas keras 5 menit (`UKUR_BATAS_KERAS_MS`).
Pengukuran yang berhenti karena penjaga mengirim apa yang sempat terkumpul;
metrik yang belum sah bernilai **0 = sentinel gagal**, dan bit dedup titik
**tidak** dinyalakan supaya bisa diulang.

Selesai → `ppg_set_enabled(false)` seketika (LED padam).

### 2.9 Cek manual vs pengukuran sesi

Pengukuran yang dipicu jari pengguna di layar home (`s_ukur_lokal`) melewati
pipeline yang sama tetapi **berhenti di layar**: tidak ada entri ring buffer,
tidak ada event. Bedakan dari `UKUR_SEKARANG` (0x05) yang diminta aplikasi
(mis. untuk kalibrasi) dan memang dijawab paket `Sampel`.

### 2.10 Generasi kalibrasi (`no_touch-callibration/`, `touchscreen/`)

Latar: `Data_Kalibrasi_Asawatch_Gabungan.md` — 21 titik glukosa dari 12 subjek
(3 subjek × 4 fase sesi makan + 9 subjek titik tunggal) dibanding glukometer
Autocheck. Rata-rata akurasi 84 %, rentang 53–98 %. Rata-rata selisih
(Autocheck − Asawatch) hanya **+6,1 mg/dL**, tetapi simpangan bakunya
**~16,3 mg/dL** dengan outlier −37 dan +35. Kesimpulan yang diambil: masalah
utamanya **derau per titik** (gerak / kontak longgar / saturasi pada kulit
berbeda), bukan bias sistematis. Empat perubahan menyasar itu:

**a. Median-of-3 pada R dan PI** (`median_r_pi()`), sebelum
`estimate_glucose`/`estimate_bp`:

- Rata-rata 5 jendela (`push_smoothed`) tidak menolak outlier — satu jendela
  rusak tetap menyeret 1/5 nilainya. Median dari 3 jendela terakhir menolaknya
  selama tidak lebih dari satu yang rusak berurutan.
- Diterapkan pada **input** (R, PI), bukan pada glukosa/tensi keluarannya:
  keduanya turunan tak-linear, jadi median hasil ≠ hasil dari median input.
- **SpO2 tetap memakai R mentah** jendela itu, karena AN6409 + offset wrist
  dikalibrasi terhadap R per jendela apa adanya.
- Dua jendela pertama tiap kontak memakai nilai mentah (belum ada riwayat),
  supaya angka tidak tertahan `--` lebih lama. Riwayat di-reset bersama
  detektor detak.

**b. Auto-Gain Control arus LED** (`agc_check()` / `agc_terapkan()`):

| Konstanta | Nilai | Arti |
|---|---|---|
| `AGC_DC_HIGH` | 200000 (~76 % skala 18-bit) | DC rata-rata di atas ini → turunkan arus sebelum benar-benar clip |
| `AGC_DC_LOW` | 60000 (2 × ambang kehadiran) | DC di bawah ini → naikkan lagi bila sempat diturunkan |
| `AGC_STEP` | 32 | Langkah per keputusan |
| `AGC_LED_MIN` | `0x40` | Lantai; jangan sampai mendekati ambang kehadiran kulit |
| `AGC_CHECK_MS` | 4000 | Bertindak paling cepat tiap 4 dtk (> `DC_SETTLE_MS`) |

- Red dan IR diatur **terpisah**: R sudah menormalkan tiap kanal terhadap
  DC-nya sendiri, jadi mengubah satu kanal hanya menggeser titik kerja, bukan
  makna rasio.
- Setiap perubahan arus **memaksa DSP mengejar ulang**: `dcInit = false`,
  `reset_beat_detector()`, `contactStartMs = now` (settle 3 dtk berlaku lagi).
  Statistik sesi **sengaja tidak** di-reset — masih kontak fisik yang sama.
- Dievaluasi di luar `windowGate` (tidak menunggu `bpmValid`) karena saturasi
  harus dikoreksi sesegera mungkin; evaluasi pertama otomatis jatuh setelah DC
  konvergen karena `s_agcLastMs` diset saat kontak dimulai.
- Motivasi: pada arus penuh, kulit tipis/pucat bisa mendekati saturasi ADC
  18-bit (262143), yang membengkokkan AC/DC jauh lebih dari derau biasa.

**c. `G0` 82,1 → 88,2** — bias rata-rata +6,1 dari 21 titik. Komentar kode
sendiri menegaskan ini **tidak** memperbaiki outlier, hanya menggeser pusat
sebaran; yang menyasar outlier adalah (a). `G1`–`G3` **tidak** diubah karena
dataset hanya mencatat glukosa keluaran vs acuan, bukan fitur mentah (PI/R/HR)
per titik — slope tidak bisa di-refit dari data itu.

**d. Offset kalibrasi glukosa per unit** (`aw_kalibrasi_glukosa_*`, NVS):

- Diset **hanya lewat konsol serial** `glu_offset <±N mg/dL>` (negatif bila
  Asawatch cenderung lebih tinggi dari acuan); **bukan** bagian protokol BLE,
  jadi tidak bisa diatur dari aplikasi. Alasannya: opcode kawat baru mewajibkan
  perubahan dokumen normatif + Flutter di PR yang sama, sementara model glukosa
  masih eksperimental dan offset ini baru berguna untuk pengujian lab per unit.
- Diterapkan di `ukur_selesai()` tepat sekali ke pembacaan mentah, seperti
  tensi: sentinel 0 tidak disentuh, hasil diklem 1..65535.
- Kalau nanti dibutuhkan dari app: jalurnya sama seperti `SET_KALIBRASI`
  (opcode baru + dokumen + versi protokol).

Yang **tidak** berubah di generasi kalibrasi: konfigurasi sensor awal, gerbang
kontak, filter DC, detektor detak, jendela/rentang plausible, formula SpO2 dan
offset wrist, koefisien tensi, penghalusan 5 jendela, gerbang selesai.

---

## 3. Ringkasan konstanta

| Konstanta | Nilai | Berkas |
|---|---|---|
| `IR_PRESENCE_THRESHOLD` | 30000 | `ppg.cpp` |
| `CONTACT_DEBOUNCE_MS` | 250 | `ppg.cpp` |
| `DC_ALPHA` / `DC_SETTLE_MS` | 0,95 / 3000 | `ppg.cpp` |
| `SLOW_ALPHA` | 0,97 | `ppg.cpp` |
| envelope decay / threshold | 0,98–0,02 / 0,5 × envelope | `ppg.cpp` |
| `MIN_IBI_MS` / IBI maks | 300 / 2000 ms | `ppg.cpp` |
| `MIN_STABLE_BEATS` | 3 | `ppg.cpp` |
| `SPO2_WINDOW_MS` / `_MIN` | 1000 / 8 sampel | `ppg.cpp` |
| `R_MIN/MAX_PLAUSIBLE` | 0,3 / 1,15 | `ppg.cpp` |
| `PI_MIN/MAX_PLAUSIBLE` | 0,02 / 15 % | `ppg.cpp` |
| `SPO2_MIN_PLAUSIBLE` | 70 % | `ppg.cpp` |
| `SPO2_WRIST_OFFSET` | 16,8 | `ppg.cpp` |
| `G0..G3` | 82,1 (dasar) · 88,2 (kalibrasi) / −15 / 0,3 / 20 | `ppg.cpp` |
| [kalibrasi] `MEDIAN_N` | 3 | `ppg.cpp` |
| [kalibrasi] `AGC_DC_HIGH` / `_LOW` / `_STEP` / `_LED_MIN` / `_CHECK_MS` | 200000 / 60000 / 32 / 0x40 / 4000 | `ppg.cpp` |
| `SBP0..3`, `DBP0..3` | 120 / 0,3 / −3 / 15 ; 80 / 0,2 / −2 / 10 | `ppg.cpp` |
| `SMOOTH_N` | 5 | `ppg.cpp` |
| `UKUR_MIN_DETAK` / `UKUR_MIN_MS` | 10 / 10000 | `aw_jam.cpp` |
| `UKUR_TANPA_KONTAK_MS` / `UKUR_BATAS_KERAS_MS` | 90000 / 300000 | `aw_jam.cpp` |
| Kalibrasi tensi: `offsetMaksimum` / `sebaranMaksimum` / `jedaAntarPutaran` / `masaBerlaku` | 30 mmHg / 12 mmHg / 60 dtk / 28 hari | `sesi_makan.dart` |

---

## 4. Status kalibrasi per metrik (apa adanya)

### 4.1 Detak jantung
Algoritmik; tidak ada koefisien untuk dikalibrasi. Yang memengaruhi akurasi:
kualitas kontak (jam rapat), gerakan, dan ambang `IR_PRESENCE_THRESHOLD`.

### 4.2 SpO2 — offset situs wrist, **1 titik data**
`SPO2_WRIST_OFFSET = 16,8` berasal dari satu perbandingan (2026-07-29): sesi
wrist menghasilkan ~83,1 % (mustahil), sesi jari di waktu berdekatan ~99,9 %.
Ini koreksi offset, **bukan kalibrasi statistik**; tidak ada jaminan konstan
untuk kondisi lain. Wajib diganti dengan koreksi dari banyak titik, idealnya
dibanding pulse oximeter medis di pergelangan yang sama.

### 4.3 Glukosa — bias saja yang terkoreksi, slope nol data
- **Dasar** (`no_touch/`): `G0` 90,0 → 82,1 dari satu perbandingan (jam 108,9
  vs GCHb 101 mg/dL, 2026-07-29).
- **Kalibrasi** (`no_touch-callibration/`, `touchscreen/`): `G0` → 88,2 dari
  rata-rata selisih 21 titik / 12 subjek vs Autocheck (2026-09-11). Ini tetap
  koreksi **bias** saja; simpangan baku 16 mg/dL menunjukkan derau per titik
  jauh lebih besar dari biasnya. Ditambah offset per unit lewat konsol serial
  (§2.10 d).
- `G1`, `G2`, `G3` adalah angka tebakan awal di **kedua** generasi — dataset
  tidak memuat PI/R/HR per titik, jadi tidak bisa di-refit.
- Bahkan setelah kalibrasi sungguhan, tidak ada dasar fisiologis tervalidasi
  yang menghubungkan rasio Red/IR dengan glukosa darah.

### 4.4 Tekanan darah — koefisien nol data + offset per orang
Koefisien `SBP*`/`DBP*` belum pernah dibandingkan dengan tensimeter. Yang ada
adalah **offset per orang dari manset** (§5), yang mengoreksi hasil regresi
placeholder itu — jadi yang sebenarnya "dikalibrasi" hanyalah intercept-nya,
per pengguna.

---

## 5. Prosedur kalibrasi tekanan darah (yang sudah ada)

Satu-satunya jalur kalibrasi yang sudah terimplementasi ujung ke ujung.
Metode manset berulang mengikuti pola alat sejenis (Samsung Health Monitor).

```
[APLIKASI: kalibrasi_tekanan_darah_page]                        [JAM]                 [BACKEND]
 1. persiapan: pilih pergelangan tempat jam dipakai (sisi)
 2. manset di lengan BERLAWANAN dengan jam
    (manset mengembang menyumbat nadi di pergelangan bawahnya)
 3. satu tombol: ── UKUR_SEKARANG (0x05) ──────────────────▶  ukur (pipeline §1, tanpa offset)
    sekaligus tekan START tensimeter                          ── Sampel(sesiId nol) ──▶
 4. angka jam DISEMBUNYIKAN sampai angka tensimeter diketik
    (melihat angka jam dulu membuat orang "membetulkan" ketikannya)
 5. offset = referensi − jam   (sistolik & diastolik, per putaran)
 6. putaran kedua ditawarkan, tidak diwajibkan; jeda ≥ 60 dtk (manset yang
    langsung dipompa ulang membaca terlalu tinggi); hasil = MEDIAN putaran
 7. validasi:
      masukAkal : |offset| ≤ 30 mmHg   → gagal: "periksa manset dan jam"
      konsisten : sebaran ≤ 12 mmHg    → gagal: "ukur ulang saat lebih tenang"
 8. ── SET_KALIBRASI (0x06): int16 offset_sis, int16 offset_dia ──▶  simpan ke NVS
                                                                     (aw_kalibrasi_set)
 9. simpan riwayat lokal (tabel_kalibrasi + tabel_putaran_kalibrasi, drift)
10. ── POST /api/v1/kalibrasi / POST /sinkron ──────────────────────────────────▶ tabel `kalibrasi`
```

Aturan yang menempel pada prosedur ini:

- **Diterapkan di jam, bukan di aplikasi** (`ukur_selesai()`): `sis += osis`,
  `dia += odia`, diklem 1..255, dan **tidak menyentuh sentinel 0**. Akibatnya
  paket `Sampel` sudah membawa angka terkoreksi; aplikasi dan backend tidak
  mengoreksi lagi.
- **Satu orang, satu pergelangan.** Offset diturunkan dari manset satu orang;
  salah untuk siapa pun selain dia. Ganti akun di HP → `hapusSemua()` di
  aplikasi, **tetapi offset di flash jam tidak ikut terhapus** (jam tidak
  punya konsep akun); pengguna baru harus mengalibrasi ulang, dan barulah
  offset lama tertimpa.
- **Kedaluwarsa 28 hari** (`masaBerlaku`) — aplikasi yang mengingatkan; jam
  tetap memakai offset yang ada sampai ditimpa.
- **Riwayat, bukan satu baris**: offset yang melonjak antar kalibrasi adalah
  informasi (manset kendur, lengan tidak setinggi jantung), jadi tabel di
  aplikasi dan backend menyimpan semua putaran.
- Jam melaporkan `bit1 kalibrasi tersimpan` di karakteristik Status.

---

## 6. Prosedur kalibrasi yang **belum ada** dan apa yang dibutuhkan

Untuk glukosa dan koefisien tensi belum ada prosedur sama sekali. Yang perlu
disiapkan kalau mau mengerjakannya:

**Data yang dikumpulkan** — firmware sudah mencetak pasangan fitur bila
`NET_DEBUG` aktif (1×/2 dtk): `BPM, SpO2, Glukosa*, TD*, [PI, R]`. Catat
berpasangan dengan alat acuan:

| Metrik | Acuan | Jumlah minimal | Syarat |
|---|---|---|---|
| Glukosa | glukometer / GCHb | ≥ 15–20 titik, **lintas level** (puasa, 1 jam, 2 jam pascamakan) | semua di pergelangan, jam yang sama, kontak baik |
| Tensi | tensimeter manset, lengan berlawanan, bersamaan | ≥ 15–20 titik lintas orang & keadaan | protokol §5 langkah 2–3 |
| SpO2 | pulse oximeter medis di pergelangan yang sama | ≥ 10 titik, termasuk kondisi rendah kalau mungkin | — |

**Pelajaran dari putaran 2026-09-11**: dataset 21 titik hanya mencatat
*keluaran* glukosa vs acuan, sehingga yang bisa dikoreksi cuma `G0`. Putaran
berikutnya harus mencatat **PI, R, HR per titik** (log `NET_DEBUG` sudah
mencetaknya) — tanpa itu `G1`–`G3` tidak akan pernah bisa di-refit.

**Cara memperbarui** — regresi linear biasa terhadap fitur (PI, HR−70, R)
menghasilkan `G0..G3` / `SBP0..3` / `DBP0..3` baru; `SPO2_WRIST_OFFSET`
diganti rata-rata selisih (atau regresi pada R kalau offsetnya ternyata tidak
konstan). Semua koefisien adalah `static double` di `ppg.cpp`; mengubahnya
berarti **flash ulang** — tidak ada jalur OTA maupun opcode untuk mengirim
koefisien. Yang bisa diatur tanpa flash ulang hanya dua offset: tensi lewat
`SET_KALIBRASI` (BLE, dari app) dan glukosa lewat `glu_offset` (konsol serial,
generasi kalibrasi saja). Kalau kalibrasi per perangkat/per orang diperlukan
dari app, opsi yang paling kecil biayanya adalah opcode serupa `SET_KALIBRASI`.

**Yang tidak bisa diperbaiki dengan kalibrasi** — glukosa dari PPG Red/IR
tidak punya dasar fisiologis tervalidasi; hasil regresi terbaik pun hanya
menunjukkan korelasi pada populasi data itu. Nyatakan ini di setiap laporan
yang memakai angkanya.

---

## 7. Gejala umum dan penyebabnya

| Gejala | Kemungkinan penyebab | Lihat |
|---|---|---|
| Layar `--` terus, detak 0 | kontak lepas/goyang, atau `peakEnvelope` terkontaminasi transien (reset tidak terjadi) | §2.2, §2.4 |
| BPM terkunci di ~15–25 | threshold terkunci ke ayunan napas — `slowAC` tidak bekerja | §2.4 |
| SpO2 mendekati 0 atau > 100, glukosa ribuan | jendela lolos tanpa gerbang plausible | §2.5 |
| Angka pertama muncul sangat lama | settle/jendela dihitung dalam sampel, bukan waktu (12,5 sampel/dtk) | §2.3, §2.5 |
| Empat kartu muncul tidak bersamaan | gerbang `bpmValid` dilepas dari jendela | §2.5 |
| Pengukuran "selesai" dalam 5 detik dengan nadi cepat | `UKUR_MIN_MS` dilepas | §2.8 |
| Tensi selalu meleset konstan untuk satu orang | belum dikalibrasi manset, atau offset milik pengguna lama masih di flash | §5 |
| `140/150` | jaring `dia ≤ sis − 10` dilepas | §2.6 |
| Satu titik meleset jauh (±35) padahal titik lain wajar | jendela tercemar gerak/kontak; generasi dasar tidak punya median | §2.10 a |
| Angka "berdenyut" naik-turun tiap beberapa detik pada kulit tertentu | AGC berosilasi di tepi ambang — periksa `AGC_CHECK_MS`/histeresis `DC_HIGH`–`DC_LOW` | §2.10 b |
| Glukosa berbeda antar unit dengan orang yang sama | `glu_offset` per unit berbeda / belum diset | §2.10 d |
