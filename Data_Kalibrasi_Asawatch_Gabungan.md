# Data Kalibrasi Asawatch (Sensor MAX30102) vs Autocheck

Berikut adalah gabungan data pengujian perangkat Asawatch (menggunakan sensor optik MAX30102) dibandingkan dengan perangkat referensi medis (Autocheck). 

Data ini mencakup:
- Pengujian multi-fase pada 3 subjek.
- Pengujian titik tunggal pada 9 subjek.

**Catatan Instruksi untuk Claude Code:**
Fokus utama adalah meminta bantuan modifikasi pada algoritma (kode `.ino`) mikrokontroler untuk mereduksi variansi akurasi dan membuat pembacaan PPG lebih stabil terhadap karakteristik individu yang berbeda.

---

## 1. Data Pengujian Multi-Fase (3 Subjek Laki-Laki)
Pengujian dilakukan dalam 4 fase: Baseline (sebelum makan), Sesudah Makan, 1 Jam Setelah Makan, dan 2 Jam Setelah Makan.

### Subjek 1: Hubbert (Laki-Laki)
- **Baseline:** Autocheck 72 mg/dL | Asawatch 89 mg/dL *(Akurasi: 76.39%)*
- **Sesudah Makan:** Autocheck 79 mg/dL | Asawatch 42 mg/dL *(Akurasi: 53.16%)*
- **1 Jam +:** Autocheck 89 mg/dL | Asawatch 81 mg/dL *(Akurasi: 91.01%)*
- **2 Jam +:** Autocheck 103 mg/dL | Asawatch 87 mg/dL *(Akurasi: 84.47%)*

### Subjek 2: Rajah (Laki-Laki)
- **Baseline:** Autocheck 102 mg/dL | Asawatch 92 mg/dL *(Akurasi: 90.20%)*
- **Sesudah Makan:** Autocheck 89 mg/dL | Asawatch 81 mg/dL *(Akurasi: 91.01%)*
- **1 Jam +:** Autocheck 76 mg/dL | Asawatch 95 mg/dL *(Akurasi: 75.00%)*
- **2 Jam +:** Autocheck 92 mg/dL | Asawatch 57 mg/dL *(Akurasi: 61.96%)*

### Subjek 3: Mas Titan (Laki-Laki)
- **Baseline:** Autocheck 82 mg/dL | Asawatch 77 mg/dL *(Akurasi: 93.90%)*
- **Sesudah Makan:** Autocheck 87 mg/dL | Asawatch 93 mg/dL *(Akurasi: 93.10%)*
- **1 Jam +:** Autocheck 111 mg/dL | Asawatch 90 mg/dL *(Akurasi: 81.08%)*
- **2 Jam +:** Autocheck 82 mg/dL | Asawatch 86 mg/dL *(Akurasi: 95.12%)*

---

## 2. Data Pengujian Titik Tunggal (9 Subjek Baru)
Pengujian dilakukan 1 kali pengukuran per subjek.

### Kelompok Laki-Laki
1. **Azhari:** Autocheck 113 mg/dL | Asawatch 90 mg/dL *(Akurasi: 79.65%)*
2. **Vinchent:** Autocheck 97 mg/dL | Asawatch 87 mg/dL *(Akurasi: 89.69%)*
3. **Hilal:** Autocheck 80 mg/dL | Asawatch 82 mg/dL *(Akurasi: 97.50%)*
4. **Satria:** Autocheck 89 mg/dL | Asawatch 87 mg/dL *(Akurasi: 97.75%)*
5. **Sugeng:** Autocheck 96 mg/dL | Asawatch 85 mg/dL *(Akurasi: 88.54%)*

### Kelompok Perempuan
1. **Anin:** Autocheck 80 mg/dL | Asawatch 83 mg/dL *(Akurasi: 96.25%)*
2. **Keysa:** Autocheck 93 mg/dL | Asawatch 105 mg/dL *(Akurasi: 87.10%)*
3. **Maira:** Autocheck 74 mg/dL | Asawatch 96 mg/dL *(Akurasi: 70.27%)*
4. **Caca:** Autocheck 106 mg/dL | Asawatch 79 mg/dL *(Akurasi: 74.53%)*

---

## 3. Ringkasan Statistik & Evaluasi
- **Total Subjek Uji:** 12 Orang (8 Laki-laki, 4 Perempuan)
- **Total Titik Pengukuran:** 21 Titik
- **Rata-rata Akurasi Keseluruhan:** 84.18%
- **Rentang Deviasi (Min-Max):** 53.16% hingga 97.75%
- **Analisis Masalah:** Adanya *outlier* yang parah (misal: pengukuran sesudah makan pada Hubbert dan 2 jam+ pada Rajah) menunjukkan bahwa metode ekstraksi sinyal mentah saat ini sangat sensitif terhadap *noise*, pergerakan, atau perbedaan saturasi pantulan cahaya (ketebalan kulit/pigmen).

## 4. Kebutuhan Revisi Kode (.ino)
Mohon bantu saya untuk merancang atau memperbaiki baris kode dengan menambahkan fitur berikut:
1. **Digital Signal Processing (DSP):** Penerapan filter (seperti Kalman atau Moving Average) di dalam loop untuk menghaluskan fluktuasi sinyal optik sebelum dihitung ke rumus glukosa.
2. **Auto-Gain Control:** Pengaturan dinamis untuk `setPulseAmplitudeRed()` agar intensitas cahaya LED bisa naik-turun secara otomatis menyesuaikan kualitas tangkapan photodiode pada berbagai jenis kulit.
3. **Mekanisme Kalibrasi Personal:** Struktur kode untuk menyimpan nilai referensi awal sebagai konstanta *offset* bagi pengguna spesifik.
