# AsaWatch

Firmware jam tangan **ESP32-C6 + LVGL 8.3**. Jam ini berperan sebagai
**sensor + buffer + penampil**: mengukur PPG (detak jantung, SpO2, dan
metrik glukosa/tensi eksperimental), menyimpan hasilnya di ring buffer NVS,
lalu mengirimkannya ke aplikasi Flutter lewat BLE. Semua kecerdasan
(verdict, tren, penjadwalan sesi) ada di sisi aplikasi, bukan di firmware.

## Struktur repo

Repo ini berisi dua varian firmware yang sudah berpisah kodenya dan
sebaiknya tidak dicampuradukkan tanpa sengaja:

| Folder | Board | Layar |
|---|---|---|
| [`touchscreen/`](touchscreen) | Waveshare ESP32-C6-Touch-LCD-1.69 | 240x280, ST7789V2 + CST816T (sentuh) |
| [`no_touch/`](no_touch) | ESP32-C6-LCD-1.69 | 240x280, ST7789V2 saja (tanpa chip sentuh) |

Masing-masing folder punya `CLAUDE.md` sendiri dengan detail lengkap:
cara build & flash, protokol BLE, aturan konteks/thread, invarian yang
tidak boleh dibalik, dan skrip generator aset.

## Keamanan

`config.h` di kedua varian memuat kredensial Wi-Fi dan API key
OpenWeatherMap dalam teks polos. Repo ini bersifat **privat** karena itu —
jangan dijadikan publik tanpa membersihkan kredensial tersebut terlebih
dahulu.
