/*
 * Konfigurasi yang harus diisi sendiri.
 *
 * PENTING: isi ketiga nilai bertanda GANTI_ di bawah sebelum flash, kalau tidak
 * Wi-Fi dan cuaca tidak akan jalan (jam tetap jalan dari RTC).
 *
 * Jangan commit file ini ke repo publik: berisi password Wi-Fi dan API key.
 */
#pragma once

/* ---------------- Wi-Fi ----------------
 *
 * DIMATIKAN. Wi-Fi di firmware ini melayani dua hal saja -- NTP dan cuaca --
 * dan yang pertama sudah punya sumber yang lebih baik: ANCHOR_WAKTU membawa
 * epoch dari HP dan tm_terapkan_epoch_utc() menuliskannya ke RTC persis seperti
 * NTP, di SETIAP koneksi (dokumen 4.2). Jadi yang tersisa milik Wi-Fi sendiri
 * cuma ikon cuaca.
 *
 * Harganya jauh lebih mahal daripada itu. AP yang tidak ada membuat net_task
 * menjalankan asosiasi yang gagal berulang-ulang, dan pemindaian aktif di 2,4
 * GHz itu berbagi radio dengan BLE. Iklan tetap keluar -- ia cuma TX sekali
 * tembak -- tetapi connect request perlu jam MENDENGAR di jendela sesaat
 * sesudah paket iklan, dan jendela itulah yang hilang. Gejalanya menyesatkan:
 * jam TERLIHAT saat memindai, lalu setiap percobaan menyambung timeout.
 *
 * Kredensialnya sengaja tidak dihapus, supaya menyalakannya lagi cukup satu
 * angka. Baca dulu gerbang dan backoff di net.cpp sebelum melakukannya.
 */
#define AW_PAKAI_WIFI   0

#define WIFI_SSID       "asawatch"
#define WIFI_PASSWORD   "12345678"

/* ---------------- OpenWeatherMap ----------------
 * API key gratis: https://home.openweathermap.org/api_keys
 * OWM_CITY memakai format "Kota,KodeNegara". Alternatif pakai koordinat:
 * ganti OWM_QUERY di weather.cpp menjadi "lat=..&lon=..".
 */
#define OWM_API_KEY     "8b41c3dffb9f25e1a2a82cb011447f00"
/* HARUS "Surakarta,ID", jangan "Solo,ID". Keduanya menjawab HTTP 200, tapi
 * "Solo,ID" di basis data OWM menunjuk desa Solo di Flores (121.12, -8.80) --
 * sekitar 1300 km dari Solo/Surakarta Jawa Tengah (110.83, -7.56). Verifikasi
 * langsung: Solo,ID -> 19.04 C, Surakarta,ID -> 31.65 C.
 * Kalau ragu, pakai koordinat: ganti "q=" jadi "lat=-7.5561&lon=110.8317". */
#define OWM_CITY        "Surakarta,ID"

/* ---------------- Zona waktu ----------------
 * WIB = UTC+7. WITA = 8, WIT = 9. Indonesia tidak memakai DST.
 */
#define TZ_OFFSET_SEC   (7 * 3600)
#define TZ_DST_SEC      0

/* ---------------- Interval ---------------- */
#define WEATHER_PERIOD_MS   (30UL * 60UL * 1000UL)  /* cuaca: 30 menit       */
#define NTP_RESYNC_MS       (6UL * 3600UL * 1000UL) /* NTP ulang: 6 jam      */
#define RTC_RESYNC_MS       (60UL * 1000UL)         /* baca RTC: 1 menit     */
#define WIFI_RETRY_MS       (60UL * 1000UL)         /* coba sambung lagi     */
#define WIFI_RETRY_MAX_MS   (15UL * 60UL * 1000UL)  /* batas atas backoff    */
#define WIFI_TIMEOUT_MS     (8UL * 1000UL)          /* lama satu percobaan   */

/* Backoff kalau permintaan cuaca gagal. WAJIB ada: tanpa jeda yang membesar,
 * kegagalan berulang (mis. key belum aktif -> HTTP 401) membuat board memanggil
 * API tiap siklus task (2 detik) = ~30 panggilan/menit, mendekati batas 60/menit
 * paket gratis dan berisiko membuat key kena rate-limit. */
#define WEATHER_RETRY_MIN_MS  (30UL * 1000UL)       /* jeda gagal pertama    */
#define WEATHER_RETRY_MAX_MS  (15UL * 60UL * 1000UL) /* batas atas backoff   */

/* ---------------- Baterai ----------------
 * GPIO0 adalah satu-satunya pin ADC1 yang tidak dipakai LCD (GPIO1..6 terpakai),
 * jadi di situlah pembagi tegangan baterai board ini.
 *
 * BATT_DIVIDER = Vbaterai / Vpin. Nilai 2.0 berarti pembagi 1:2 (mis. 100k/100k).
 * KALIBRASI: lihat angka "raw" di baris [batt] pada Serial, lalu ukur tegangan
 * baterai dengan multimeter. BATT_DIVIDER = Vukur / Vraw.
 */
#define BATT_ADC_PIN    0

/* Dikalibrasi memakai multimeter sungguhan, di titik yang SAMA dengan yang
 * dilihat pembagi ADC -- bukan menebak titik regulasi charger.
 *
 * Nominal pembagi 200k/100k = 3.00, tapi itu mengabaikan toleransi resistor dan
 * offset ADC. Kalibrasi PERTAMA (2.97) menebak charger sudah rata di 4.200 V
 * persis saat pengisian dianggap selesai (raw 1414 mV -> 4200/1414 = 2.97) --
 * tanpa multimeter sungguhan, hanya asumsi dari datasheet Li-Po.
 *
 * DUA titik pengukuran, bukan satu -- dan keduanya penting: saat sel dicas,
 * ADA jatuh tegangan (IR drop) sepanjang kabel/konektor akibat arus cas, jadi
 * "di pin BATT board" (titik yang sama dengan pembagi ADC) dan "langsung di
 * terminal sel" BEDA -- terukur di board ini: pin 4,19 V, sel 4,17-4,18 V,
 * selisihnya MENGECIL seiring arus cas mengecil mendekati penuh. Yang benar
 * dipakai untuk kalibrasi PEMBAGI adalah PIN BATT, karena resistor pembagi ada
 * di board dan menyambung ke situ -- bukan ke sel di seberang kabel.
 *
 * Kalibrasi KEDUA (dibuang, lihat di bawah): 4,14 V dipasangkan dengan raw
 * 1381 mV sebelum titik "pin vs sel" ini dibedakan -- tidak jelas titik mana
 * yang diukur, jadi tidak dipakai (rasionya 2.998, meleset ~1% dari yang
 * dipastikan sesudahnya).
 *
 * Kalibrasi KETIGA (dipakai): dua pengukuran independen di PIN BATT, keduanya
 * 4,19 V, bertepatan dengan raw 1382 dan 1382-1383 mV -- rasio 3,0318 dan
 * 3,0307, SEPAKAT satu sama lain sampai 0,04%. Dipakai rata-ratanya, 3,031.
 *
 * Kenapa presisi ini penting: kurva memetakan apa pun >= BATT_CV_MV di sisi sel
 * menjadi 100%. Rasio yang kekecilan membuat baterai yang SUDAH 100% masih
 * terbaca di bawahnya -- angka mentok padahal selnya sudah penuh. Rasio yang
 * kebesaran membuat sebaliknya: "selalu 100%" padahal baru terkuras sedikit.
 *
 * Untuk kalibrasi ulang: BATT_DIVIDER = Vukur / raw yang tercetak, diukur DI
 * PIN BATT board (titik yang sama dengan pembagi ADC), bukan di terminal sel
 * kalau sedang dicas -- keduanya baru sama persis saat arus cas nol (penuh
 * benar-benar, atau baterai sedang tidak dicas sama sekali). */
#define BATT_DIVIDER    3.031f

/* ---- Persen saat DICAS (battery.cpp) ----
 *
 * Selama kabel tertancap DAN arus masih mengalir, tegangan di pin BUKAN
 * tegangan sel: charger menaikkannya sebesar arus x hambatan (sel + kabel +
 * konektor). Terukur di board ini: mencolok kabel menggeser tegangan +100 mV
 * dalam 1-2 detik, sebelum sel menerima muatan sedikit pun. Kurva Li-Po
 * membaca +100 mV itu sebagai ~+8% -- itulah "cepat penuh" dan itulah
 * "ngedrop" 8-15% saat kabel dicabut.
 *
 * TERBUKTI LANGSUNG dengan multimeter (dua kali, di board ini): begitu arus
 * cas mengecil mendekati penuh, jatuh tegangan itu ikut mengecil sampai
 * hilang -- pada titik itu tegangan di "pin BATT" dan di terminal sel SAMA
 * PERSIS (diukur 4,09 V lalu 4,19 V, keduanya sama di kedua sisi). Artinya di
 * fase CV yang sudah plateau, tegangan mentah SUDAH BOLEH dipercaya apa
 * adanya -- tidak perlu ditebak lagi pakai penghitung waktu.
 *
 *   BATT_CHG_IR_MV       koreksi yang dikurangkan dari tegangan HANYA selagi
 *                        arus masih nyata mengalir (belum plateau) -- lihat
 *                        cabang `cv` di battery_update().
 *   BATT_CV_MV           tegangan (sisi baterai) yang dianggap sudah fase CV
 *                        -- begitu tercapai, persen dibaca LANGSUNG dari
 *                        tegangan (tanpa koreksi, tanpa nunggu), naik seketika
 *                        kalau tegangan mendukung, turun pelan kalau ternyata
 *                        cuma derau. Tidak wajib tepat: rata-nya tegangan
 *                        (naik < 6 mV per 3 menit di atas BATT_CV_PLATEAU_MIN_MV)
 *                        juga dianggap CV.
 *   BATT_CHG_MAX_PCT_MIN persen tertinggi yang boleh bertambah per menit
 *                        SEBELUM plateau (fase CC, saat koreksi IR di atas
 *                        masih dipakai) -- bukan lagi berlaku sesudah plateau,
 *                        karena tegangan sendiri sudah jadi bukti langsung.
 *                        Batas fisik: sel tidak bisa terisi lebih cepat dari
 *                        arus chargernya. Untuk sel 1500 mAh, 1%/menit setara
 *                        ~900 mA -- di atas arus charger board semacam ini,
 *                        jadi ia hanya pagar, bukan penentu. Sel 1500 mAh
 *                        pada ~500 mA butuh ~3 jam dari kosong; "penuh dalam
 *                        beberapa puluh menit" pasti artefak. */
#define BATT_CHG_IR_MV        100
#define BATT_CV_MV            4170
/* Batas bawah "plateau" yang dianggap fase CV. Plateau di bawah ini (charger
 * atau port USB yang tertahan di ~4,05-4,10 V) BUKAN sel yang penuh, jadi tidak
 * boleh dipercaya sebagai tegangan final. Sengaja sedikit di bawah BATT_CV_MV
 * supaya toleransi BATT_DIVIDER (+-1% dari nilai terukur) tidak membuat sel
 * yang benar-benar penuh terlewat. */
#define BATT_CV_PLATEAU_MIN_MV 4150
#define BATT_CHG_MAX_PCT_MIN  1

/* Tampilkan log diagnostik jam/cuaca/baterai di Serial. */
#define NET_DEBUG 1
