#include <Arduino.h>
#include "battery.h"
#include "config.h"

/* ---- median dari N sampel ----
 * Median dipilih, bukan rata-rata: burst transmit Wi-Fi membuat tegangan ambles
 * beberapa milidetik. Rata-rata akan ikut tertarik turun oleh pencilan itu,
 * median mengabaikannya sama sekali.
 */
#define SAMPLES 15

static int  s_raw_mv   = 0;    /* di pin ADC, sebelum dikali rasio */
static int  s_batt_mv  = 0;    /* setelah rasio + EMA             */
static int  s_percent  = 0;
static bool s_valid    = false;
static int  s_spread   = 0;    /* max-min jendela sampel terakhir */
static int  s_counts   = 0;    /* hitungan ADC mentah 0..4095 -- deteksi saturasi */

/* ---- dasar perhitungan persen: MEDIAN minimum-slot 3 menit ----
 *
 * Median sampel di atas hanya mampu menolak pencilan yang LEBIH PENDEK dari
 * jendelanya (~2.5 detik). Sag akibat radio Wi-Fi bukan pencilan pendek: ia
 * bertahan selama seluruh upaya sambung, jadi seluruh jendela berada di dalam
 * satu keadaan beban dan medianya ikut pindah.
 *
 * Terukur di board ini: radio aktif -> 1365 counts, radio mati -> 1382 counts.
 * Selisih 17 counts = 18 mV di pin = 53 mV di baterai = sekitar 4% pada kurva
 * di dekat penuh.
 *
 * Tegangan dikumpulkan per slot 5 detik (nilai TERENDAH slot itu), 36 slot = 3
 * menit, lalu dasar persennya adalah MEDIAN slot-slot itu.
 *
 * Versi sebelumnya memakai MINIMUM seluruh slot. Itu bagus melawan sag radio
 * tetapi menjadikan persen tawanan satu peristiwa: satu pengukuran PPG (LED
 * menarik arus puluhan mA selama belasan detik) atau satu burst BLE menyeret
 * minimum 3 menit ke bawah, angkanya turun 1-3%, dan naik lagi hanya kalau
 * selisih >= 2% -- jadi angka hanya bisa turun dan tidak pernah pulih. Itulah
 * "kadang ngedrop". Median tidak punya cacat itu: peristiwa yang menempati
 * kurang dari separuh jendela (6 slot dari 36 untuk satu pengukuran PPG, 8 dari
 * 60 detik siklus retry Wi-Fi) tidak menggeser hasilnya sama sekali, sedangkan
 * beban yang menetap tetap tercermin karena mengisi lebih dari separuh slot.
 *
 * Versi sebelum itu memakai floor ABADI (terendah yang pernah teramati) dan
 * cacat lain: floor terpasang di 4143 mV saat sel hampir penuh, sementara
 * pemulihan butuh +100 mV = 4243 mV -- di atas batas fisik Li-Po 4200 mV.
 * Jendela bergerak tidak pernah terpaku, ia ikut NAIK saat baterai diisi.
 */
#define WMIN_SLOTS      36            /* 36 x 5 s = 3 menit */
#define WMIN_SLOT_MS    5000UL
static uint16_t s_wmin[WMIN_SLOTS];
static int      s_wmin_n = 0, s_wmin_i = 0;
static int      s_slot_min = 0;       /* minimum slot 5 s yang sedang berjalan */
static uint32_t s_slot_ms  = 0;
static int      s_base_mv  = 0;       /* median jendela -> dasar persen */
static bool     s_charging = false;
static bool     s_usb_bukti = false;   /* ada bukti pasti kabel tertancap (lihat battery_usb_pasti) */
static const char *s_sebab = "-";      /* aturan yang terakhir menyalakan s_charging */

/* ---- persen: TIGA keadaan, bukan satu kurva ----
 *
 * Kurva Li-Po memetakan tegangan ISTIRAHAT ke kapasitas. Saat kabel tertancap
 * tegangan di pin adalah tegangan charger (sel + arus x hambatan dalam), bukan
 * tegangan sel, jadi memakai kurva yang sama di kedua keadaan pasti salah di
 * salah satunya:
 *
 *   - mengisi: angka menyentuh 90%+ dalam beberapa menit ("cepat penuh"), lalu
 *   - dicabut: tegangan jatuh ~100 mV dan angka ikut ambles 8-15% seketika.
 *
 * Karena itu:
 *   BATERAI  persen = kurva(median), digeser 1% per langkah (lihat di bawah).
 *   MENGISI  persen = kurva(median - BATT_CHG_IR_MV), hanya boleh NAIK, tidak
 *            lebih cepat dari BATT_CHG_MAX_PCT_MIN per menit, mentok 99%.
 *            Di fase CV tegangan rata dan tidak memuat informasi apa pun; angka
 *            merayap dari 92% ke 99% menurut waktu, dan 100% baru diizinkan
 *            setelah BATT_CV_FULL_MIN menit.
 *   Colok/cabut: persen yang sedang tampil DIBAWA lewat peralihan; jendela
 *            tegangan dikosongkan karena isinya milik keadaan yang lama. */
#ifndef BATT_CHG_LEBIH_PCT
#define BATT_CHG_LEBIH_PCT 4          /* batas atas saat dicas: kurva(tegangan) + ini */
#endif
#define PCT_TURUN_MS   5000UL         /* baterai: 1% turun per 5 s (<= 12%/mnt) */
#define PCT_NAIK_MS    15000UL        /* baterai: 1% naik per 15 s              */
#define PCT_NAIK_MIN   3
#define PCT_LANJUT_PCT 12             /* boot: lanjut dari nilai tersimpan kalau selisih <= ini */              /* baterai: naik hanya kalau selisih >= ini */
#define CV_PLATEAU_MV  6              /* naik < ini per 3 mnt di >= 4100 mV = CV */
#define CV_START_PCT   92             /* kurva(4200 - 100 mV): awal merayap CV   */
static int      s_percent_init = 0;
static int      s_tersimpan    = 0;   /* persen tersimpan di NVS dari sesi sebelumnya (0 = tidak ada) */
static uint32_t s_pct_ms       = 0;   /* langkah persen terakhir                */
static uint32_t s_cv_ms        = 0;   /* akumulasi waktu di fase CV             */
static uint32_t s_upd_ms       = 0;   /* pemanggilan battery_update sebelumnya  */
static int      s_chg_ir_mv    = BATT_CHG_IR_MV;  /* koreksi tegangan saat mengisi   */
static int      s_ir_pre_mv    = 0;   /* tegangan tepat sebelum langkah colok    */
static uint32_t s_ir_ms        = 0;   /* kapan langkah colok terdeteksi (0=tidak) */

/* ---- deteksi "sedang dicolok": tiga aturan yang saling menambal ----
 *
 * Ambang di bawah bukan tebakan; semuanya dari log probe di board ini
 * (perintah konsol `sag`/`saglog` di touch.ino, yang tetap ada untuk mengukur
 * ulang kalau selnya diganti):
 *
 *   dicolok  t=22..62s   SAG = -2, +0, +0        berat 4104..4105 mV
 *   baterai  t=82..182s  SAG = +6,+10,+9,+9,+13  berat 4001..3967 mV
 *   dicolok  t=202s      SAG = +1                berat 4104 mV
 *
 * Jarak antara dua gugus itu 5 mV dengan pengulangan +-2 mV. Sempit dalam
 * angka absolut, tetapi konsisten -- dan yang menanggung beban keputusan
 * sehari-hari sebenarnya deteksi langkah 100 mV, bukan sag ini. */
#define STEP_SLOTS       8      /* 8 x ~500 ms = 4 detik  */
#define STEP_MV          50     /* langkah colok/cabut terukur >100 mV */
/* Ambang sag (mV, di sisi baterai, DESIMAL -- lihat probe_rata_f()).
 *
 * Versi sebelumnya memakai dua ambang bulat, <= 3 "dicolok" dan >= 5 "baterai",
 * dengan zona mati 4 mV di antaranya, dikalibrasi saat backlight masih duty 204
 * (sag baterai +6..+13 mV). Backlight sekarang duty 153 (~75% arusnya), jadi sag
 * di baterai turun ke ~+4,5..+10 dan pada sel hangat berhambatan rendah (habis
 * dicas lama) mendarat di zona mati -- atau di bawah 3. Akibatnya dua gejala
 * nyata: (1) sesudah kabel dicabut saat penuh, ikon charge TERSANGKUT walau PWR
 * diklik berulang, karena tak satu pun probe melewati 5; (2) satu probe <= 3
 * di baterai langsung menyalakan ikon + toast, lalu probe berikutnya
 * memadamkannya lagi ("tiba-tiba keluar animasi charge lalu hilang").
 *
 * Sekarang keputusan dari BUKTI yang menumpuk, bukan satu pembacaan:
 *   sag <= SAG_COLOK_YAKIN   satu probe cukup menyalakan (sumber teregulasi
 *                            tidak punya penjelasan lain; terukur -2..+1)
 *   sag <= SAG_COLOK         "mirip dicolok"; DUA probe berturut-turut menyalakan
 *
 * Nilai -0,5 / 1,0 dipilih dari simulasi (sel 1500 mAh, sag baterai 2,4..9 mV,
 * derau ADC 1,5 mV di pin): 0 ikon charge palsu di baterai dalam 90 menit,
 * dibanding 6-16 kali untuk aturan lama (<= 3 langsung menyalakan); ikon yang
 * tersangkut sesudah kabel dicabut saat penuh hilang dalam ~21 detik (aturan
 * lama tidak pernah, dan berkedip 8-44 kali); colok saat penuh dikenali ~20 detik.
 * Sag baterai di bawah ~2 mV (sel berhambatan sangat rendah + layar sangat redup)
 * memang tidak bisa dibedakan dari sumber teregulasi oleh probe apa pun. 
 *   sag >= SAG_BATERAI       jelas di luar rentang tercolok (-0,9..+1,5 sesudah
 *                            kompensasi); satu probe cukup memadamkan
 *   DUA probe berturut-turut > SAG_BUKAN_COLOK -> padam
 * Kasus terakhir yang menyembuhkan ikon tersangkut: tidak ada probe pun yang
 * perlu sampai 5 mV, cukup tiga-tiganya bukan pembacaan sumber teregulasi. */
#ifndef SAG_COLOK_YAKIN_F
#define SAG_COLOK_YAKIN_F  -0.5f
#endif
#ifndef SAG_COLOK_F
#define SAG_COLOK_F        1.0f
#endif
/* Memadamkan butuh bukti yang LEBIH KUAT daripada batas menyalakan (histeresis):
 * bacaan tercolok mendarat di -0,9..+1,5 sesudah kompensasi, jadi batas "bukan
 * tercolok" yang sama dengan batas menyalakan (1,0) membuat derau sekecil apa
 * pun memadamkan ikon saat kabel masih tertancap -- terbukti di simulasi (ikon
 * berkedip 2-6 kali dalam 20 menit tercolok). */
#ifndef SAG_BATERAI_F
#define SAG_BATERAI_F      3.0f      /* satu probe cukup memadamkan            */
#endif
#ifndef SAG_BUKAN_COLOK_F
#define SAG_BUKAN_COLOK_F  1.8f      /* dua probe berturut-turut > ini memadamkan */
#endif
/* Bias polaritas untuk probe TUNGGAL (belum ada pasangannya). Terukur di jam ini
 * saat tercolok, 16 probe: layar-ON +0,7..+2,7 (rata-rata +1,3), layar-OFF
 * -1,1..-3,1 (rata-rata -2,2) -- sesudah tiap perubahan layar ada transien turun
 * ~1,7 mV, jadi ON terbaca kebesaran dan OFF terbaca kekecilan. Sesudah dikurangi
 * bias, semua probe tercolok mendarat di -0,9..+1,5 (rata-rata ~0). Tanpa ini
 * satu probe layar-OFF di baterai (sag 3 - 2,2 = ~1) terbaca "mirip tercolok",
 * dan klik PWR pertama tidak pernah menjadi bukti "di baterai". Probe yang punya
 * pasangan berlawanan memakai rata-ratanya (transien meniadakan diri), bukan ini. */
#ifndef SAG_BIAS_ON_F
#define SAG_BIAS_ON_F      1.2f
#endif
#ifndef SAG_BIAS_OFF_F
#define SAG_BIAS_OFF_F    -2.2f
#endif
#define SAG_PROBE_N        64          /* sampel rata-rata pembacaan sesudah beban berubah */
#define PROBE_SETTLE_MS  300

/* ---- Jalur cepat "baru dicolok" ----
 *
 * Langkah 50 mV di atas bekerja pada EMA + median-15, dua penghalus yang sengaja
 * lambat. Akibatnya dua hal yang terukur di simulasi (sel 1500 mAh, charger 500
 * mA): lonjakan 100 mV baru dikenali 2,5 detik setelah dicolok, 75 mV butuh 3,5
 * detik, dan lonjakan <= 50 mV TIDAK PERNAH dikenali oleh langkah itu -- ia
 * menunggu tren 3 menit (150 detik) sebelum ikon dan animasi charge muncul.
 * Itulah "kadang delay": makin kecil lonjakannya (sel makin penuh, hambatan sel
 * makin kecil), makin lama.
 *
 * Jalur ini membaca sampel segar tiap tick (median 3, tanpa cincin dan EMA) dan
 * membandingkan TIGA tick terakhir dengan median 8 tick sebelumnya. Kenaikan
 * harus bertahan semua tiga tick: pelepasan beban sesaat (layar mati +13 mV, LED
 * PPG padam ~12 mV) bukan hanya lebih kecil dari ambang, ia juga tidak akan
 * melewati konfirmasi. Ambang 40 mV berada di atas dua pelepasan itu ditambah
 * satu sama lain, dan di bawah lonjakan colok terukur (+100..137 mV).
 * Hanya untuk NAIK: mencabut tetap lewat langkah 50 mV yang lama, karena
 * "kok ikon charge hilang" tidak dikeluhkan dan pelepasan beban radio yang
 * panjang bisa menyerupai kenaikan. */
#define FAST_N        12      /* 8 tick pembanding + 3 tick konfirmasi + 1 sela  */
#define FAST_OLD      8
#define FAST_CONFIRM  3
#define FAST_STEP_MV  40
static uint16_t s_fast[FAST_N];
static int      s_fast_n = 0, s_fast_i = 0;

static uint16_t s_step[STEP_SLOTS];
static int      s_step_n = 0, s_step_i = 0;

static float    s_probe_v0f    = 0;   /* tegangan sebelum langkah beban (EMA desimal) */
static float    s_ema_f        = 0;   /* EMA desimal per tick, acuan "sebelum" probe   */
static bool     s_ema_ok       = false;
static float    s_sag_f        = 0;
static float    s_sag_pakai_f  = 0;   /* nilai terkompensasi/berpasangan yang dipakai keputusan */
static uint8_t  s_pl_hist      = 0;   /* 3 probe terakhir, bit0 = terbaru, 1 = mirip dicolok */
static uint8_t  s_pl_n         = 0;
static uint8_t  s_bk_hist      = 0;   /* 2 probe terakhir, 1 = jelas bukan tercolok (> SAG_BUKAN_COLOK_F) */
static float    s_prev_sag_f   = 0;   /* probe sebelumnya (untuk rata-rata pasangan) */
static bool     s_prev_berat   = false;
static uint32_t s_prev_ms      = 0;
static uint32_t s_probe_seq    = 0;   /* bertambah tiap probe selesai (untuk log)    */
static bool     s_probe_berat_terakhir = false;
static uint32_t s_probe_ms     = 0;
static bool     s_probe_nunggu = false;
static bool     s_probe_ke_berat = false;
static int      s_sag_mv       = 0;
static bool     s_sag_ada      = false;
static uint32_t s_probe_us     = 0;
static uint32_t s_tren_sah_ms  = 0;   /* tren dibisukan sampai 3 mnt setelah ini */
static uint32_t s_step_naik_ms = 0;   /* langkah-naik terakhir; menggerbang sag besar */

/* Acuan sejak nyala. Tanpa ini tidak ada cara membedakan dua sebab yang sama
 * sekali berbeda ketika persen tidak bergerak:
 *   - tegangan memang belum turun (kurva Li-Po sangat datar di dekat penuh,
 *     dan 10 menit pada sel 1000 mAh hanya sekitar 2%)
 *   - tegangan turun tapi angkanya tertelan plafon 4200 mV = 100% karena
 *     BATT_DIVIDER terlalu tinggi
 * Selisih dalam mV memperlihatkan keduanya, jauh sebelum persen bergerak. */
static int      s_first_mv = 0;
static uint32_t s_first_ms = 0;

/* Kurva pelepasan Li-Po 1 sel. Hubungan tegangan-kapasitas jauh dari linear --
 * peta linear 3.3-4.2 V akan salah besar di tengah rentang. Titik-titik ini
 * diinterpolasi linear di antaranya. */
typedef struct { int mv; int pct; } curve_pt_t;
static const curve_pt_t CURVE[] = {
  { 4200, 100 }, { 4100, 92 }, { 4000, 85 }, { 3950, 78 },
  { 3900,  70 }, { 3850, 62 }, { 3800, 55 }, { 3750, 47 },
  { 3700,  40 }, { 3650, 33 }, { 3600, 25 }, { 3550, 18 },
  { 3500,  12 }, { 3450,  8 }, { 3400,  5 }, { 3300,  2 },
  { 3000,   0 },
};
static const int CURVE_N = sizeof(CURVE) / sizeof(CURVE[0]);

static int mv_to_percent(int mv) {
  if (mv >= CURVE[0].mv) return 100;
  if (mv <= CURVE[CURVE_N - 1].mv) return 0;
  for (int i = 0; i < CURVE_N - 1; i++) {
    if (mv <= CURVE[i].mv && mv > CURVE[i + 1].mv) {
      int dv = CURVE[i].mv  - CURVE[i + 1].mv;
      int dp = CURVE[i].pct - CURVE[i + 1].pct;
      return CURVE[i + 1].pct + ((mv - CURVE[i + 1].mv) * dp + dv / 2) / dv;
    }
  }
  return 0;
}

static int cmp_int(const void *a, const void *b) {
  return (*(const int *)a) - (*(const int *)b);
}

/* Rata-rata N sampel berurutan, dalam mV di sisi BATERAI, DESIMAL. Rata-rata
 * bukan median: median atas pembacaan bulat tetap bulat (kelipatan ~3 mV di sisi
 * baterai), padahal sag yang dibedakan cuma beberapa mV -- dan derau ADC (sebaran
 * 2-4 mV) justru bekerja sebagai dither yang membuat rata-ratanya sub-langkah. */
static float probe_rata_f(int n) {
  long jumlah = 0;
  for (int k = 0; k < n; k++) jumlah += analogReadMilliVolts(BATT_ADC_PIN);
  return (float)jumlah * BATT_DIVIDER / (float)n;
}

/* Median seluruh slot penuh + slot yang sedang berjalan. Kurang dari 4 slot
 * (baru boot, atau jendela baru dikosongkan oleh colok/cabut) belum punya arti
 * statistik, jadi dipakai nilai penghalusan sesaat -- tidak diam pada nilai lama
 * yang milik keadaan daya sebelumnya. */
static int basis_mv(int sesaat) {
  int v[WMIN_SLOTS + 1];
  int n = 0;
  for (int k = 0; k < s_wmin_n; k++) v[n++] = s_wmin[k];
  v[n++] = s_slot_min;
  if (n < 4) return sesaat;
  qsort(v, n, sizeof(int), cmp_int);
  return v[n / 2];
}

/* Kosongkan jendela 3 menit: isinya milik keadaan daya yang baru berakhir. */
static void kosongkan_jendela(void) {
  s_wmin_n = 0;
  s_wmin_i = 0;
  s_slot_min = s_batt_mv;
  s_slot_ms  = millis();
}

/* Naik/turun 1% per selang, menuju target. Dipanggil tiap 500 ms. */
static void geser_persen(int target, uint32_t selang_ms, bool boleh_turun) {
  const uint32_t now = millis();
  if ((uint32_t)(now - s_pct_ms) < selang_ms) return;
  if (target > s_percent)                    { s_percent++; s_pct_ms = now; }
  else if (boleh_turun && target < s_percent){ s_percent--; s_pct_ms = now; }
}

void battery_begin(void) {
  /* Atenuasi penuh supaya rentang ukur mencapai ~3.3 V: dengan pembagi 1:2
   * baterai 4.2 V muncul sebagai 2.1 V di pin. */
  analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);
  pinMode(BATT_ADC_PIN, INPUT);
}

/* Buffer cincin: jendela median dibangun LINTAS pemanggilan, bukan di dalam
 * satu pemanggilan. Versi pertama kode ini mengambil 15 sampel berjarak 2 ms
 * sekaligus -- 30 ms memblokir, setara satu periode refresh LVGL penuh, dan
 * cukup untuk menunda pembacaan touch yang datanya cepat hilang. Sekarang tiap
 * pemanggilan hanya ambil 3 sampel berurutan (~0.3 ms) lalu isi cincin, jadi
 * jendelanya justru jadi lebih panjang (~2.5 detik pada laju 500 ms) yang
 * malah lebih baik untuk menolak burst transmit Wi-Fi yang berskala milidetik. */
static int  s_ring[SAMPLES];
static int  s_ring_n = 0;
static int  s_ring_i = 0;

/* ---- riwayat 1 menit/sampel, disimpan di RAM ----
 * Serial tidak bisa dibaca saat board jalan dari baterai (USB dicabut), jadi
 * board mencatat sendiri. Begitu USB dicolok lagi board TIDAK reset -- ia hanya
 * mulai mengisi -- sehingga riwayat ini masih utuh dan bisa dibaca. Inilah satu-
 * satunya cara melihat apakah tegangan benar-benar turun saat memakai baterai. */
/* 30 menit. Cukup untuk tes pelepasan: cabut USB, pakai 20-30 menit, colok lagi
 * (board TIDAK reset, hanya mulai mengisi) lalu baca riwayatnya. Itu satu-satunya
 * cara melihat tegangan saat benar-benar jalan dari baterai, karena serial hanya
 * hidup saat USB tertancap -- dan saat USB tertancap sel sedang diisi. */
#define HIST_N 30
static uint16_t s_hist[HIST_N];
static int      s_hist_n = 0;
static int      s_hist_i = 0;
static uint32_t s_hist_ms = 0;

void battery_update(void) {
  s_counts = analogRead(BATT_ADC_PIN);   /* mentah: 4095 = saturasi */
  int segar[3];
  for (int k = 0; k < 3; k++) {
    segar[k] = analogReadMilliVolts(BATT_ADC_PIN);          /* dikalibrasi eFuse */
    s_ring[s_ring_i] = segar[k];
    s_ring_i = (s_ring_i + 1) % SAMPLES;
    if (s_ring_n < SAMPLES) s_ring_n++;
  }
  /* Median 3 sampel segar tick ini -> jalur cepat "baru dicolok". */
  {
    int a = segar[0], b = segar[1], c = segar[2], m;
    m = (a < b) ? ((b < c) ? b : ((a < c) ? c : a))
                : ((a < c) ? a : ((b < c) ? c : b));
    /* Rata-rata desimal, bukan median bulat: sag yang dicari beberapa mV dan
     * ADC-nya berkuantisasi ~3 mV di sisi baterai, jadi hanya rata-rata atas
     * derau yang bisa mengurai di bawah satu langkah kuantisasi. alpha 0,3
     * (tetapan waktu ~1,4 detik) supaya sisa langkah beban sebelumnya <2%
     * sebelum probe berikutnya. */
    const float rata = (segar[0] + segar[1] + segar[2]) * BATT_DIVIDER / 3.0f;
    if (s_ema_ok) s_ema_f += 0.3f * (rata - s_ema_f);
    else          { s_ema_f = rata; s_ema_ok = true; }
    s_fast[s_fast_i] = (uint16_t)(m * BATT_DIVIDER + 0.5f);
    s_fast_i = (s_fast_i + 1) % FAST_N;
    if (s_fast_n < FAST_N) s_fast_n++;
  }

  int s[SAMPLES];
  memcpy(s, s_ring, s_ring_n * sizeof(int));
  qsort(s, s_ring_n, sizeof(int), cmp_int);
  s_raw_mv = s[s_ring_n / 2];                    /* median */
  s_spread = s[s_ring_n - 1] - s[0];             /* seberapa berisik jendelanya */

  int mv = (int)(s_raw_mv * BATT_DIVIDER + 0.5f);

  if (!s_valid) {
    s_batt_mv  = mv;                             /* pengukuran pertama langsung */
    s_valid    = true;
    s_slot_min = mv;
    s_slot_ms  = millis();
    s_base_mv  = mv;
  } else {
    /* EMA simetris alpha 0.2. Tidak perlu asimetris lagi: yang menahan artefak
     * beban sekarang minimum jendela di bawah, bukan penghalusan ini. */
    s_batt_mv = (s_batt_mv * 8 + mv * 2) / 10;
  }

  /* ---- minimum jendela bergerak 3 menit -> dasar persen ---- */
  if (s_batt_mv < s_slot_min) s_slot_min = s_batt_mv;

  if ((uint32_t)(millis() - s_slot_ms) >= WMIN_SLOT_MS) {
    s_slot_ms = millis();
    s_wmin[s_wmin_i] = (uint16_t)s_slot_min;
    s_wmin_i = (s_wmin_i + 1) % WMIN_SLOTS;
    if (s_wmin_n < WMIN_SLOTS) s_wmin_n++;
    s_slot_min = s_batt_mv;                      /* mulai slot berikutnya */
  }

  const uint32_t now = millis();
  const uint32_t dt  = s_upd_ms ? (uint32_t)(now - s_upd_ms) : 0;
  s_upd_ms = now;
  const bool was_charging = s_charging;
  int  step_pra_mv = 0;                 /* tegangan sebelum langkah colok, kalau ada */

  /* Bukti pasti dari luar modul ini (boot tanpa tombol PWR, atau board masih
   * hidup setelah latch dilepas): berlaku SEKETIKA, tanpa menunggu tegangan.
   * Dijalankan SETELAH was_charging dicatat supaya lewat blok peralihan di
   * bawah (persen dikoreksi, jendela dikosongkan) seperti colok biasa.
   * s_step_naik_ms dipasang agar probe sag tidak bisa memadamkannya selama 3
   * menit pertama -- gerbang yang sama dengan colok yang terdeteksi langkah. */
  if (s_usb_bukti) {
    s_usb_bukti = false;
    if (!s_charging) {
      s_charging     = true;
      s_sebab        = "usb-pasti";
      s_step_naik_ms = now;
      s_tren_sah_ms  = now;
      s_sag_ada      = false;
    }
  }

  /* ================= Apakah jam sedang dicolok? =================
   * Tiga aturan, dijalankan dari yang paling lemah ke yang paling kuat supaya
   * yang kuat menang dalam pemanggilan yang sama.
   *
   * Versi pertama menjalankannya terbalik dan itu bug yang nyata: aturan tren
   * dievaluasi TERAKHIR, jadi sesaat setelah kabel dicabut, deteksi langkah
   * memang menyetel false -- lalu tren yang masih memuat kenaikan selama
   * mengisi tadi langsung menyetelnya true lagi di baris berikutnya. Ikonnya
   * macet menyala sampai jendela 3 menit itu habis, dan tiap probe sag yang
   * datang di sela itu pun ditimpa lagi. Gejalanya persis "stuck di kondisi
   * charge terus". */

  /* ---- 1. Tren naik 3 menit: memegang fase CC ----
   * HANYA boleh menyalakan, tidak pernah memadamkan. Ia cuma benar di satu
   * fase: saat mengisi dari sel kosong, charger bekerja sebagai sumber ARUS,
   * jadi langkah beban tetap menghasilkan sag persis seperti di baterai dan
   * aturan 2 akan salah menyimpulkan "tidak mengisi". Yang tidak bisa
   * dipalsukan di fase itu adalah tegangan yang naik terus.
   *
   * Kebalikannya tidak berlaku -- tegangan yang tidak naik bukan bukti tidak
   * mengisi (di CV memang tidak naik) -- karena itu tidak ada cabang else.
   *
   * s_tren_sah_ms adalah penawar kemacetan di atas: setiap langkah membisukan
   * tren selama satu panjang jendela penuh, supaya ia tidak pernah menjawab
   * berdasarkan slot yang terkumpul SEBELUM keadaan dayanya berubah. Sengaja
   * memakai stempel waktu: kosongkan_jendela() memang dijalankan di tiap
   * colok/cabut, tetapi rule 2 dan 3 bisa mengubah s_charging tanpa peralihan
   * yang menguras jendela, dan tren tetap tidak boleh menjawab dari slot lama. */
  if (s_wmin_n >= WMIN_SLOTS &&
      (uint32_t)(millis() - s_tren_sah_ms) >= (uint32_t)WMIN_SLOTS * WMIN_SLOT_MS) {
    int oldest = s_wmin[s_wmin_i];               /* slot berikut = yang tertua */
    int newest = s_wmin[(s_wmin_i + WMIN_SLOTS - 1) % WMIN_SLOTS];
    if (newest - oldest >= 20) { s_charging = true; s_sebab = "tren"; }
  }

  /* ---- 2. Probe sag: memegang fase CV ----
   * Di sinilah tren buta total: saat sel hampir penuh charger meregulasi di
   * 4,2 V dan tegangannya RATA, jadi tidak ada tren untuk dilihat. Yang tetap
   * berbeda adalah tanggapannya terhadap beban -- charger menahan, baterai
   * ambles. Terukur di board ini dengan backlight sebagai beban: dicolok
   * -2..+1 mV, di baterai +6..+13 mV (duty 204; sekarang lebih kecil, lihat
   * SAG_*_F di atas).
   *
   * Jendela 300 ms cukup: tegangan mengikuti langkah beban dalam milidetik,
   * jauh lebih cepat daripada relaksasi kimia sel yang berskala detik. */
  if (s_probe_nunggu && (uint32_t)(millis() - s_probe_ms) >= PROBE_SETTLE_MS) {
    s_probe_nunggu = false;
    const uint32_t t0 = micros();
    const float v1f = probe_rata_f(SAG_PROBE_N);
    s_probe_us = micros() - t0;      /* satu-satunya pembacaan mahal yang tersisa */
    const float sag_f = s_probe_ke_berat ? (s_probe_v0f - v1f) : (v1f - s_probe_v0f);
    s_sag_f   = sag_f;
    s_sag_mv  = (int)(sag_f + (sag_f >= 0 ? 0.5f : -0.5f));
    s_sag_ada = true;
    s_probe_seq++;
    s_probe_berat_terakhir = s_probe_ke_berat;

    /* Rata-rata PASANGAN. Terukur di jam ini saat tercolok: probe layar-OFF
     * -1,1..-2,6 mV, layar-ON +0,7..+1,4 mV -- bertanda berlawanan, karena
     * sesudah tiap perubahan layar ada transien turun ~1,5 mV yang mengurangi
     * pembacaan "naik" (OFF) dan menambah pembacaan "turun" (ON). Sag sungguhan
     * sama untuk keduanya, sedangkan transien berlawanan tanda, jadi rata-rata
     * satu probe ON dan satu OFF yang berdekatan meniadakan transien itu.
     * Tanpa ini satu probe OFF di baterai (sag 3 mV - 2 mV transien = 1 mV)
     * terbaca "mirip dicolok". */
    float sag_pakai = sag_f - (s_probe_ke_berat ? SAG_BIAS_ON_F : SAG_BIAS_OFF_F);
    if (s_prev_ms && (uint32_t)(millis() - s_prev_ms) < 90000UL &&
        s_prev_berat != s_probe_ke_berat)
      sag_pakai = 0.5f * (sag_f + s_prev_sag_f);
    s_sag_pakai_f = sag_pakai;
    s_prev_sag_f = sag_f;
    s_prev_berat = s_probe_ke_berat;
    s_prev_ms    = millis();

    const bool mirip_colok = sag_pakai <= SAG_COLOK_F;
    s_pl_hist = (uint8_t)(((s_pl_hist << 1) | (mirip_colok ? 1 : 0)) & 7);
    s_bk_hist = (uint8_t)(((s_bk_hist << 1) | (sag_pakai > SAG_BUKAN_COLOK_F ? 1 : 0)) & 3);
    if (s_pl_n < 3) s_pl_n++;

    /* Menyalakan: yakin dalam satu probe, atau dua probe berturut-turut yang
     * sama-sama mirip sumber teregulasi. */
    if (sag_pakai <= SAG_COLOK_YAKIN_F || (s_pl_n >= 2 && (s_pl_hist & 3) == 3)) {
      s_charging = true;
      s_sebab    = "probe-sag";
    } else {
      /* Memadamkan. Sag BESAR tidak simetris -- ia muncul baik di baterai
       * MAUPUN saat mengisi di fase CC (charger bekerja sebagai sumber ARUS),
       * jadi dua pengaman tetap: langkah-naik terakhir harus sudah cukup lama
       * berlalu supaya aturan 1 sempat punya jendela penuh, dan tegangan tidak
       * boleh sedang naik (>= 6 mV per 3 menit = masih fase CC). Tanpa
       * keduanya, mencolok charger ke baterai kosong memadamkan ikonnya
       * sendiri beberapa detik kemudian. */
      const bool dua_bukan    = (s_pl_n >= 2 && s_bk_hist == 3);
      const bool pasti_baterai = sag_pakai >= SAG_BATERAI_F;
      bool masih_naik = false;
      if (s_wmin_n >= WMIN_SLOTS)
        masih_naik = (s_wmin[(s_wmin_i + WMIN_SLOTS - 1) % WMIN_SLOTS] -
                      s_wmin[s_wmin_i]) >= CV_PLATEAU_MV;
      if ((pasti_baterai || dua_bukan) && !masih_naik &&
          (uint32_t)(millis() - s_step_naik_ms) >=
              (uint32_t)WMIN_SLOTS * WMIN_SLOT_MS) {
        s_charging = false;
      }
    }
  }

  /* ---- 3. Langkah cepat: kabel dicolok / dicabut ----
   * Bukti paling langsung yang ada, karena itu dijalankan terakhir dan menang
   * atas dua aturan di atas. Terukur di board ini: mencabut 4105 -> 4001 mV,
   * mencolok lagi 3967 -> 4104 mV. Seratus milivolt ke atas, dalam satu-dua
   * detik. Tidak ada apa pun dalam pemakaian normal yang menggeser tegangan
   * sebesar itu secepat itu, jadi ambang 50 mV punya margin dua kali lipat dan
   * tetap jauh di atas sag beban (9 mV). */
  s_step[s_step_i] = (uint16_t)s_batt_mv;
  s_step_i = (s_step_i + 1) % STEP_SLOTS;
  if (s_step_n < STEP_SLOTS) s_step_n++;

  bool naik_cepat = false;
  if (s_fast_n >= FAST_N) {
    int lama_v[FAST_OLD];
    for (int k = 0; k < FAST_OLD; k++) lama_v[k] = s_fast[(s_fast_i + k) % FAST_N];
    qsort(lama_v, FAST_OLD, sizeof(int), cmp_int);
    const int med = lama_v[FAST_OLD / 2];
    int terendah = 100000;                       /* terendah dari tick konfirmasi */
    for (int k = FAST_N - FAST_CONFIRM; k < FAST_N; k++) {
      int v = s_fast[(s_fast_i + k) % FAST_N];
      if (v < terendah) terendah = v;
    }
    naik_cepat = (terendah - med) >= FAST_STEP_MV;
  }

  if (s_step_n >= STEP_SLOTS) {
    int lama = s_step[s_step_i];
    int baru = s_step[(s_step_i + STEP_SLOTS - 1) % STEP_SLOTS];
    if (baru - lama >= STEP_MV || naik_cepat) {
      s_charging     = true;
      s_sebab        = naik_cepat ? "cepat" : "langkah";
      step_pra_mv    = lama;
      s_step_naik_ms = millis();
      s_tren_sah_ms  = millis();     /* bisukan tren: slotnya milik keadaan lama */
      s_sag_ada      = false;
    } else if (lama - baru >= STEP_MV) {
      s_charging     = false;
      s_tren_sah_ms  = millis();
      s_sag_ada      = false;
    }
  }

  /* ================= Persen =================
   * Lihat "persen: TIGA keadaan" di atas untuk alasan tiap cabang. */

  /* Peralihan colok/cabut. Persen yang tampil TIDAK disentuh sama sekali --
   * hanya jendela tegangan dan penghitung CV yang dikosongkan.
   *
   * Sengaja tidak ada koreksi "boot di USB" (angka awal dibaca dari tegangan
   * charger kalau kabel sudah tertancap sebelum jam menyala, jadi ia tinggi
   * ~5-10% sampai selnya mengejar). Sudah dicoba dan dibuang: satu-satunya
   * sinyal yang ada untuk kasus itu adalah probe sag, yang punya positif-palsu
   * sesekali di baterai, dan positif-palsu itu bisa bertahan menit-menit
   * (pemadamannya digerbang 3 menit sejak boot, dan tanpa ketukan layar tidak
   * ada probe baru). Menurunkan angka atas dasar sinyal itu menciptakan persis
   * "ngedrop" yang mau dihilangkan -- simulasi menunjukkan 74% -> 58% di baterai
   * biasa. Angka yang terlalu tinggi sedikit dan menyusut sendiri lebih murah
   * daripada angka yang jatuh tanpa sebab. */
  if (!was_charging && s_charging) {
    /* Besar koreksi tegangan (arus x hambatan dalam) bergantung pada charger dan
     * sel yang dipakai, jadi diukur sendiri dari lonjakan saat dicolok -- BUKAN
     * dipatok. Ditunda 10 detik: saat langkah baru terdeteksi (melewati 50 mV)
     * EMA baru menempuh separuh jalan, dan membacanya saat itu memberi angka
     * separuh dari yang sebenarnya. Tanpa langkah (terdeteksi lewat probe atau
     * tren) dipakai nilai bawaan BATT_CHG_IR_MV. */
    s_pl_hist = 0; s_bk_hist = 0; s_pl_n = 0; s_prev_ms = 0;
    s_chg_ir_mv = BATT_CHG_IR_MV;
    s_ir_ms     = step_pra_mv ? now : 0;
    s_ir_pre_mv = step_pra_mv;
    kosongkan_jendela();
    s_cv_ms  = 0;
    s_pct_ms = now;
  } else if (was_charging && !s_charging) {
    s_pl_hist = 0; s_bk_hist = 0; s_pl_n = 0; s_prev_ms = 0;
    s_ir_ms = 0;
    kosongkan_jendela();
    s_cv_ms  = 0;
    s_pct_ms = now;
  }

  const int basis = basis_mv(s_batt_mv);
  s_base_mv = basis;

  if (!s_percent_init) {
    /* Boot di USB yang DIKETAHUI (s_charging sudah true di sini): tegangan
     * pertama adalah tegangan charger, jadi dikoreksi sejak angka pertama. */
    const int v = mv_to_percent(basis - (s_charging ? s_chg_ir_mv : 0));
    /* Lanjutkan dari persen yang TAMPIL sebelum jam mati, kalau masih masuk akal
     * (dalam PCT_LANJUT_PCT dari hitungan tegangan sekarang). Bacaan boot satu
     * kali derau dan tegangan berbeban, sedangkan angka tersimpan adalah hasil
     * berjam-jam penghalusan. Di luar rentang itu sel jelas berubah selama mati
     * (dicas atau terkuras) dan tegangan yang dipercaya. */
    if (s_tersimpan > 0 && abs(v - s_tersimpan) <= PCT_LANJUT_PCT) s_percent = s_tersimpan;
    else                                                           s_percent = v;
    s_percent_init = 1;
    s_pct_ms       = now;
  } else if (s_charging) {
    /* Fase CV: tegangan menempel di plafon charger, jadi tidak ada lagi yang
     * bisa dibaca darinya. Dikenali dari dua hal supaya tidak bergantung pada
     * ketepatan BATT_DIVIDER: sudah di atas BATT_CV_MV, atau rata (kenaikan
     * jendela 3 menit di bawah CV_PLATEAU_MV) di wilayah 4100 mV ke atas. */
    int naik = 999;                                /* belum ada jendela penuh */
    if (s_wmin_n >= WMIN_SLOTS)
      naik = s_wmin[(s_wmin_i + WMIN_SLOTS - 1) % WMIN_SLOTS] - s_wmin[s_wmin_i];
    if (s_ir_ms && (uint32_t)(now - s_ir_ms) >= 10000UL) {
      int ir = s_batt_mv - s_ir_pre_mv;
      s_chg_ir_mv = ir < 50 ? 50 : (ir > 150 ? 150 : ir);   /* langkah terdeteksi >= 50 */
      s_ir_ms = 0;
    }
    const bool cv = basis >= BATT_CV_MV ||
                     (basis >= BATT_CV_PLATEAU_MIN_MV && naik < CV_PLATEAU_MV);
    if (cv) s_cv_ms += dt;

    const uint32_t penuh_ms = (uint32_t)BATT_CV_FULL_MIN * 60000UL;
    int target = mv_to_percent(basis - s_chg_ir_mv);
    if (s_cv_ms > 0) {
      uint32_t t = s_cv_ms < penuh_ms ? s_cv_ms : penuh_ms;
      int merayap = CV_START_PCT + (int)((uint64_t)(100 - CV_START_PCT) * t / penuh_ms);
      if (merayap > target) target = merayap;
    }
    if (s_cv_ms < penuh_ms && target > 99) target = 99;

    /* Batas atas fisik: angka saat dicas tidak boleh melebihi apa yang dikatakan
     * tegangan sendiri (kurva TANPA koreksi charger) lebih dari BATT_CHG_LEBIH_PCT.
     * Merayap menurut waktu hanya menebak -- kalau tegangannya sendiri tidak
     * mendukung (plateau di 4,10 V, bukan 4,2 V), angka "penuh" itu mengarang,
     * dan begitu jam dimatikan-dinyalakan angkanya dihitung ulang dari tegangan
     * lalu satu bar hilang. */
    const int atas = mv_to_percent(basis) + BATT_CHG_LEBIH_PCT;
    if (target > atas) target = atas;
    geser_persen(target, 60000UL / BATT_CHG_MAX_PCT_MIN, false);   /* hanya naik */

    /* Pengaman status "mengisi" yang tersangkut. Mencabut kabel saat sel sudah
     * penuh nyaris tidak menggeser tegangan (arus CV sudah kecil), jadi tidak ada
     * langkah untuk dilihat dan pembeda satu-satunya, probe sag, bisa tidak
     * memutuskan. Kalau angka di sini hanya boleh naik, ia membeku di 100% selama
     * sel terkuras. Yang tidak bisa dipalsukan: di charger yang sungguhan,
     * tegangan tidak mungkin di bawah kurva berbeban -- kalau kurva TANPA koreksi
     * sudah lebih rendah dari angka yang tampil, itu sel yang terkuras. */
    const int nyata = mv_to_percent(basis);
    if (nyata < s_percent - PCT_NAIK_MIN) geser_persen(nyata, PCT_TURUN_MS, true);
  } else {
    /* Turun 1% langsung tampil (itu yang dicari pemakai); naik hanya kalau
     * selisihnya nyata -- pemulihan setelah beban dilepas, bukan derau. Baterai
     * yang sedang dipakai tidak mungkin bertambah isinya, jadi kenaikan kecil
     * hanya boleh dianggap getaran ambang. */
    const int target = mv_to_percent(basis);
    if (target < s_percent)                      geser_persen(target, PCT_TURUN_MS, true);
    else if (target >= s_percent + PCT_NAIK_MIN) geser_persen(target, PCT_NAIK_MS, false);
  }

  /* Catat satu titik per menit. */
  if (!s_hist_ms || (uint32_t)(millis() - s_hist_ms) >= 60000UL) {
    s_hist_ms = millis();
    s_hist[s_hist_i] = (uint16_t)s_raw_mv;
    s_hist_i = (s_hist_i + 1) % HIST_N;
    if (s_hist_n < HIST_N) s_hist_n++;
  }
}

/* Tulis riwayat ke buf, sampel tertua dulu, satuan mV di pin. */
void battery_history(char *buf, int n) {
  int off = 0;
  buf[0] = '\0';
  for (int k = 0; k < s_hist_n && off < n - 8; k++) {
    int idx = (s_hist_i - s_hist_n + k + 2 * HIST_N) % HIST_N;
    off += snprintf(buf + off, n - off, "%u ", (unsigned)s_hist[idx]);
  }
}

int battery_history_count(void) { return s_hist_n; }

int  battery_floor_mv(void)       { return s_base_mv; }
int  battery_cv_detik(void)       { return (int)(s_cv_ms / 1000UL); }
const char *battery_sebab_mengisi(void) { return s_sebab; }
void battery_usb_pasti(void)            { s_usb_bukti = true; }
void battery_set_tersimpan(int pct)     { s_tersimpan = pct; }
bool battery_charging(void)       { return s_charging; }
int  battery_percent(void)        { return s_percent; }
int  battery_millivolts(void)     { return s_batt_mv; }
int  battery_raw_millivolts(void) { return s_raw_mv; }
bool battery_valid(void)          { return s_valid; }
int  battery_spread_mv(void)      { return s_spread; }
int  battery_raw_counts(void)     { return s_counts; }

/* ---- bacaan seketika untuk probe sag ----
 * Sengaja memakai buffer lokal, bukan s_ring: mencampurkan sampel probe ke ring
 * berarti langkah beban yang kita buat sendiri ikut menggeser persen yang tampil
 * di layar, yaitu artefak yang justru sedang kita ukur. */
#define PROBE_N 31

int battery_baca_langsung_mv(int *sebaran_pin_mv) {
  int s[PROBE_N];
  for (int k = 0; k < PROBE_N; k++) s[k] = analogReadMilliVolts(BATT_ADC_PIN);
  qsort(s, PROBE_N, sizeof(int), cmp_int);
  if (sebaran_pin_mv) *sebaran_pin_mv = s[PROBE_N - 1] - s[0];
  return (int)(s[PROBE_N / 2] * BATT_DIVIDER + 0.5f);
}

/* Dipanggil TEPAT SEBELUM duty backlight diubah, oleh layar_set() dan oleh
 * penyalaan pertama di setup().
 *
 * Tidak ada probe aktif di sini, dan itu keputusan yang disengaja: build
 * kalibrasi mengedipkan backlight sendiri tiap 20 detik, dan itu tidak bisa
 * dibiarkan di firmware yang dipakai. Ternyata memang tidak perlu -- jam sudah
 * mengubah bebannya sendiri setiap kali layar mati atau menyala, jadi
 * pengukurannya cukup MENUMPANG pada langkah yang toh sudah terjadi. Nol
 * kedipan, nol daya tambahan.
 *
 * Efek sampingnya kebetulan persis yang diinginkan: nilainya diperbarui pada
 * detik layar dinyalakan -- yaitu saat pengguna benar-benar sedang menatap
 * ikonnya. Yang tampil selalu hasil ratusan milidetik lalu, bukan tren 3 menit
 * yang basi.
 *
 * Hanya menyimpan nilai "sebelum"; battery_update() yang menyelesaikannya 300 ms
 * kemudian, sehingga fungsi ini tidak pernah memblokir lebih dari ~3 ms dan
 * aman dipanggil dari konteks loop(). */
void battery_beban_akan_berubah(bool jadi_berat) {
  if (!s_valid) return;                /* belum ada acuan; abaikan saja */

  /* Nilai "sebelum" diambil dari s_batt_mv yang SUDAH ada, bukan dari burst ADC
   * baru. Versi pertama membaca 31 sampel di sini dan itu keliru tempat: fungsi
   * ini dipanggil dari layar_set(), yang ada di jalur tekan-tombol dan
   * bangun-layar -- persis dua interaksi yang paling terasa kalau tertunda.
   * Sekarang biayanya nol, dan yang tersisa hanya satu pembacaan 300 ms
   * kemudian di battery_update(), jauh dari jalur kritis.
   *
   * Boleh dipakai karena s_batt_mv memang berarti "tegangan pada beban yang
   * berlaku sebelum ini" -- beban belum berubah saat baris ini jalan. Syaratnya
   * EMA-nya sempat mengendap, karena itu gerbang 2 detik di bawah: dua
   * transisi berturut-turut yang rapat membuat nilai "sebelum" masih separuh
   * jalan dari langkah sebelumnya, dan sag-nya jadi mengada-ada. */
  const uint32_t sekarang = millis();
  if (s_probe_ms && (uint32_t)(sekarang - s_probe_ms) < 2000UL) {
    s_probe_nunggu = false;            /* terlalu rapat -- lewati, jangan tebak */
    s_probe_ms     = sekarang;
    return;
  }

  s_probe_v0f      = s_ema_ok ? s_ema_f : (float)s_batt_mv;
  s_probe_ms       = sekarang;
  s_probe_ke_berat = jadi_berat;
  s_probe_nunggu   = true;
}

int  battery_sag_mv(void)   { return s_sag_mv; }
uint32_t battery_probe_seq(void)         { return s_probe_seq; }
float    battery_probe_sag_f(void)       { return s_sag_f; }
float    battery_probe_pakai_f(void)     { return s_sag_pakai_f; }
bool     battery_probe_berat(void)       { return s_probe_berat_terakhir; }
bool battery_sag_valid(void){ return s_sag_ada; }

uint32_t battery_probe_us(void) { return s_probe_us; }
