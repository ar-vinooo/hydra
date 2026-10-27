# Progres 19 Oktober 2026: cache shader, tes instruksi, format target, MSAA

Semua perbandingan di bawah adalah **referensi dokumentasi NVIDIA / matematika
IEEE di Python vs eksekusi GPU Apple Metal**, bukan GPU NVIDIA fisik. Header
NVIDIA (`clb197.h`, `clb1c0.h`) diunduh dari
[open-gpu-doc](https://github.com/NVIDIA/open-gpu-doc/tree/master/classes);
semua skrip menerima path-nya sebagai argumen.

## 1. Invalidasi cache shader (selesai)
Kunci cache kini memvalidasi seluruh rentang kode yang bisa dibaca decompiler
dan method invalidasi 3D/compute (`0x87`, `0x369`, `0xb2`, compute `0x5a6`)
ditangani. `tests/shader_cache_reference.py` (+ `shader_cache_reference_tests.cpp`):
184/184 kasus, bit-exact RGBA32Float, 8 nilai header diaudit. **Kontrol:**
revalidasi dimatikan sementara → runner langsung gagal; perbaikan dipulihkan →
184/184 lagi.

## 2. Instruksi Maxwell → MSL → Metal (selesai untuk set di bawah)
- `tests/maxwell_alu_reference.py` + mode file-kasus `runMaxwellAluCaseFile`
  (di `shader_coverage_tests.cpp`, binary `hydra-maxwell-alu-reference-tests`).
  8226 kasus: FADD/FMUL(+scale)/FFMA/FMNMX (neg/abs/sat/FTZ), MUFU
  (rcp, rsq, sqrt, ex2, lg2; sin/cos lewat pasangan RRO+MUFU), IADD (+neg, +SAT),
  IADD32I, LOP, LOP3 (LUT), SHL, SHR, ISCADD, IMNMX, POPC, FLO. Aritmetika float
  memakai pembulatan eksak (Fraction); hasil dibandingkan bit demi bit.
- **Temuan & perbaikan:** `IADD.SAT` tidak diimplementasikan (hasil membungkus);
  kini menjepit signed (`decoder/integer_arithmetic.cpp`, juga IADD32I).
- **Temuan sin/cos:** shader asli (dump air terjun) selalu `FMUL 2π → RRO → MUFU.SIN`;
  RRO = salin dan MUFU = `sin(x)` radian sudah benar. Skala 1/2π tanpa RRO **tidak**
  diuji dan tidak diasumsikan.
- **Batas host (tercatat di `KNOWN_GAPS`):** GPU Apple membuang sebagian denormal
  fp32 (`c + 0.0f → 0`, `c * 1.0f` tetap); Maxwell mempertahankannya tanpa `.FTZ`.
  Kasus normal lulus semua; kasus denormal dipisah ke grup `*_denormal`.
- `tests/maxwell_texture_reference.py`: TEXS 2D.LZ dan 2D.LL.DC, nearest/bilinear,
  addressing clamp/repeat/mirror/zero, depth compare (PCF bilinear), 1584 kasus.
  Bilinear dengan toleransi 1/128 (kuantisasi bobot GPU). Mutasi referensi
  (mirror→repeat) terdeteksi, jadi tes memang bisa gagal. Sampler dibuat langsung
  di Metal; pemetaan TSC→sampler tidak diuji di sini.
- FSETP/ISETP register kini diuji lewat Python; lihat checkpoint lanjutan di bawah.
- F2I/I2F kini mempunyai subset Python dan empat mode rounding; lihat lanjutan
  konversi di bawah.
- Belum: TLD/TLD4/TXD, FMZ, kelengkapan rounding non-RN di arithmetic/F2F,
  serta kelengkapan instruksi half/double lewat jalur Python.

## 3. Format render target 0x40–0x47 (selesai)
A16, AF16, AF32, A8R8, R16_A16, RF16_AF16, RF32_AF32, B8G8R8A8 (nilai diaudit dari
header), ditambah A8 (`0xF7`, sebelumnya Invalid). Tiap format memakai format Metal
dengan tata letak byte sama (nama NVIDIA menyebut komponen dari bit tertinggi):
A16→R16Unorm, AF16→R16Float, AF32→R32Float, A8R8→RG8Unorm (byte R,A),
R16_A16→RG16Unorm, RF16_AF16→RG16Float, RF32_AF32→RG32Float,
B8G8R8A8→RGBA8Unorm (byte A,R,G,B). Output fragment disusun ulang di akhir shader
(`ColorOutputSwizzle`, masuk kunci cache) dan warna+mask clear lewat
`swizzleClearColor`. `tests/color_target_format_reference.py`: byte mentah dan
clear cocok untuk semua format; dua kontrol (A8B8G8R8, RF16) lulus.
- Batas: faktor blend yang merujuk alpha memakai kanal alpha Metal, bukan kanal
  tempat alpha dipetakan (untuk format ini alpha bukan kanal fisik); blending ke
  format ini belum diuji. Konversi fp32→fp16 target render GPU Apple membulatkan
  ke nol (1 ulp dari RN-even; kontrol RF16 lama pun begitu).

## 4. MSAA dan alpha-to-coverage (belum diimplementasikan)
Tidak ada dukungan sample count, tekstur multisample, resolve, atau A2C; ini
menyentuh target render, TIC `msaa_mode`, blit 2D, pipeline, dan shader. Agar tidak
mengimplementasikan buta, kini hanya ada deteksi: `ThreeD::noteUnimplementedMultisample`
membaca `SET_ANTI_ALIAS` (0x15d0, SAMPLES 3:0) dan `SET_ANTI_ALIAS_ALPHA_CONTROL`
(0x153c, A2C bit 0, A2One bit 4), mencatat peringatan satu kali dan menghitung draw
di `HYDRA_PERF_STATS=1` ("UNIMPLEMENTED draw state"). `tests/msaa_register_audit.py`
memeriksa konstanta terhadap header. **Langkah berikut menunggu data:** jalankan
game dengan `HYDRA_PERF_STATS=1` dan laporkan baris itu.

## Regresi 19 Oktober
gpu-engine, render-hash, raster-state, texture-cache (asersi format alpha diperbarui),
shader-library, frame-trace, visual-diagnostic, blend/vertex/depth-stencil Python:
lulus. `hydra-shader-corpus-tests` crash (mutex) pada dump yang berisi BRX; crash
yang sama tanpa perubahan hari ini, jadi sudah ada sebelumnya.

## Zona waktu: footer POSIX TZ (selesai)
`parseTimeZoneBinary` ([tzif.cpp](../../src/core/horizon/services/timesrv/internal/tzif.cpp))
dulu hanya mencetak "Time zone name parsing not implemented" untuk footer
`\nEST5EDT,M3.2.0,M11.1.0\n`. Kini footer di-parse (port `tzparse` dari IANA tzcode,
format `<nama>`, `Jn`, `n`, `Mm.w.d`, `/waktu`) dan diperluas menjadi transisi
eksplisit dari 1970 sampai tabel penuh (1000 entri), ditambahkan setelah transisi
terakhir di file, persis cara tzload. Footer tanpa DST (`JST-9`) tidak menambah apa
pun (tipenya sudah ada); nama DST tanpa tanggal butuh `posixrules` dan dilewati.
`tests/timezone_reference.py` + `hydra-timezone-tests`: 15 zona dari tzdata sistem
(NY, LA, London, Paris, Dublin, Sydney, Lord Howe, Auckland, Tokyo, Kolkata, Kathmandu,
St Johns, Santiago, Sao Paulo, UTC), transisi tersimpan setelah 2020 sengaja dibuang
sehingga 2021-2100 bergantung pada footer; offset, flag DST, dan singkatan dibandingkan
dengan `zoneinfo` (sekitar 110 ribu probe per zona). Semua cocok; mutasi (offset salah
pada aturan start) membuat 10 zona gagal. Santiago hanya dibandingkan mulai 2025 karena
pemerintah Chile memundurkan jadwal 2022-2024. Ini data tzdata sistem, bukan firmware
Switch (TimeZoneBinary tidak dipakai di tes). **Tidak terkait macet Crash 4**: warning
itu juga ada di run yang berhasil.

## Crash 4: nvmap padding, cache shader, tekstur BC streaming (19 Oktober, sesi malam)
- **Macet = game abort.** `MapBufferEx` gagal (`InvalidAddress`) untuk pool NVN 0x1E9000
  yang dipetakan 0x1F0000 (dibulatkan ke big page): ekor padding tidak punya memori guest
  tetapi diperiksa. Kini hanya bagian nvmap yang dipetakan/diperiksa (`nvhost_as_gpu.cpp`,
  juga `remap`); tes `shutdown_tests --check-gpu-address-space/--check-gpu-as-lifecycle`.
  Log asli ada di `~/Library/Logs/Hydra` (config `log_output = "file"`), bukan stdout.
- **Cache shader** dinamai `registryID` GPU yang berubah antar sesi, sehingga tiap sesi
  mengompilasi ulang ribuan shader. Kini `v3-<hash nama GPU>-<hash OS>`; cache lama
  digabung ke folder baru lewat clone APFS.
- **Blok warna acak pada material streaming** (pilar, bangku, jalan, badan Crash): Unreal
  men-stream tekstur BC dengan merender blok ke target `rgba32float`/`rg32float` yang
  memakai alamat mip BC. `synchronize2DWith2D` membandingkan dimensi piksel sehingga
  gagal ("cannot synchronize 2D textures") dan tekstur BC tetap basi. Kini
  `calculateBlockTexelAlias` memetakan level BC <-> texel berukuran sama dengan blok
  (menyalin lewat jalur buffer yang sudah ada di `Texture::copyFrom`), dan bila gagal
  dipakai sinkron lewat memori guest (decode ditunda, sekali per update). Tes
  `texture_cache_tests` "BC blocks written through RGBA32 render targets" gagal pada kode
  lama dengan peringatan yang sama dengan di game dan lulus sekarang.
- Belum diverifikasi di game; foto sebelum perbaikan ada di sesi ini.

## Lanjutan: oracle Python predicate dan crash corpus

`tests/maxwell_predicate_reference.py` membandingkan model scalar Python dengan
instruksi Maxwell asli yang di-decode menjadi IR/MSL lalu dieksekusi Metal.
Referensi semantik: [NVIDIA PTX setp](https://docs.nvidia.com/cuda/parallel-thread-execution/#comparison-and-selection-instructions-setp).
PTX menjelaskan persamaan, bukan encoding SASS Maxwell; encoding memakai layout
offline seperti runner ALU sebelumnya. **Tidak memakai GPU NVIDIA fisik.**

- **499.296/499.296 cocok bit demi bit**: ISETP unsigned 35.616, signed 35.616,
  FSETP 131.712, FSETP.FTZ 296.352. Semua compare, AND/OR/XOR, NaN, infinity,
  signed zero, subnormal, batas integer, abs/neg, predicate input/inversion serta
  guard normal/terbalik. Seluruh delapan register hasil, tujuh predicate, dan
  empat flag CC diperiksa, termasuk state yang seharusnya tidak berubah.
- **Bug nyata:** sebelum perbaikan 1.024 kasus FSETP gagal karena pembandingan
  float Metal membuang subnormal. `getFloatCmp` kini memakai urutan bit integer
  dengan normalisasi ±0 dan deteksi NaN. `.FTZ` diteruskan eksplisit dari FSET
  dan FSETP, mencakup register/cbuf/immediate melalui helper bersama; Python
  baru hanya membuktikan operand register FSETP/ISETP.
- Regresi half dahulu sengaja mengganti operand F32 subnormal dengan nol untuk
  meniru keterbatasan host. Ekspektasi itu dihapus; helper bersama sekarang
  mempertahankan nilainya. Suite shader-library lengkap kembali lulus.
- Oracle ALU lama: 8.226 kasus, nol grup dengan hasil tak terduga; known gaps
  aritmetika denormal tetap berlaku. Perbaikan ini khusus perbandingan, bukan
  seluruh arithmetic fp32. Build aplikasi dan runner berhasil dengan `-j 6`.
- **Crash corpus terisolasi:** callback logger debugger dipanggil dari worker
  yang belum didaftarkan. LLDB menunjukkan `Debugger::logOnThisThread` memakai
  iterator thread yang tidak ada, kemudian `Thread::log` gagal mengunci mutex.
  Worker corpus kini register/unregister sesuai API debugger. Corpus board-live
  1.137/1.137 terkompilasi dengan Metal validation. Peringatan BRX tanpa constant
  buffer masih ada: hasil kompilasi ini bukan bukti semantik branch lengkap.

Reproduksi dan batas lebih rinci: [README predicate](../../tests/README-maxwell-predicate-reference.md).
Laporan lokal: `/tmp/hydra-predicate-before.json`,
`/tmp/hydra-predicate-final.json`, `/tmp/hydra-predicate-alu-regression.json`.
Tidak ada game/GUI yang dijalankan, commit, atau staging. Verifikasi gameplay
perbaikan BC streaming serta data pemakaian MSAA/A2C masih menunggu pengguna;
tahap instruksi dan keseluruhan grafis tetap **OPEN**.

## Lanjutan implementasi: F2I subnormal dan empat mode I2F

`tests/maxwell_conversion_reference.py` memakai semantik
[NVIDIA PTX cvt](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cvt),
Fraction/integer Python eksak, dan runner Maxwell -> IR -> MSL -> GPU Metal.
Ini tetap model referensi NVIDIA, **bukan eksekusi GPU NVIDIA fisik**; PTX tidak
mensertifikasi encoding SASS Maxwell yang dipakai runner.

- Tes awal 83.952 kasus menemukan **60 mismatch F2I** tanpa FTZ dan **899
  mismatch I2F** pada RM/RP/RZ. RN dan F2I.FTZ sudah cocok pada subset awal.
- F2I sekarang mempertahankan efek pembulatan subnormal dengan magnitude normal
  bertanda di bawah 0,5 sebelum floor/ceil/rint/trunc; FTZ menggantinya dengan
  signed zero secara eksplisit. Perbaikan berlaku untuk hasil 16/32/64-bit.
- Decoder I2F kini meneruskan field rounding. Helper bersama membentuk bit
  F16/F32/F64 langsung dari integer, termasuk signed/unsigned 64-bit, ties-even,
  carry mantissa, directed rounding, dan overflow F16. Perilaku modifier abs/neg
  yang sudah ada dipertahankan dan tetap diperiksa suite C++ lama.
- Tes diperluas menjadi **197.184/197.184 bit-exact**: F32 -> S/U16/32/64
  (33.984 tanpa FTZ + 33.984 FTZ), serta S/U8/16/32/64 -> F16/F32/F64
  (32.304 untuk masing-masing RN/RM/RP/RZ). Predicate guard/inversion, byte/half
  selector, NaN, infinity, saturasi, batas integer dan random diuji; register,
  predicate, dan CC yang tidak ditulis harus tetap sama.
- Build aplikasi dan runner berhasil (`-j 6`); shader-library lengkap, **58 grup
  PASS**, lulus dengan Metal API validation. Laporan lokal:
  `/tmp/hydra-conversion-before.json`, `/tmp/hydra-conversion-final.json`;
  log regresi `/tmp/hydra-conversion-shader-regression.log`.

Reproduksi dan batas: [README konversi](../../tests/README-maxwell-conversion-reference.md).
Belum diuji lewat Python: F2I sumber F16/F64, CC conversion, encoding cbuf/immediate,
overlap pasangan register, I2F abs/neg, dan rounding F2F. Fitur tersebut tidak
dianggap selesai oleh tes baru. Tidak menjalankan game/GUI, tidak commit/stage.

## Lanjutan: F2F lengkap per ukuran, operand dan overlap

Oracle baru `tests/maxwell_f2f_reference.py` memakai Fraction Python untuk
seluruh sembilan pasangan F16/F32/F64. Referensinya tetap
[PTX cvt NVIDIA](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cvt),
bukan GPU NVIDIA fisik atau sertifikasi encoding SASS.

- Sebelum perbaikan: **3.248 mismatch dari 50.624 kasus**. Ditemukan precision
  rounding yang tidak diteruskan, double rounding F64 -> F32 -> F16, subnormal
  yang terpengaruh host, serta SAT/FTZ yang belum diteruskan pada beberapa
  jalur F64. F2F kini memakai helper bit integer bersama, membulatkan langsung
  ke format tujuan dan menerapkan FTZ khusus input/output F32.
- Raw mode 1 pada perubahan ukuran berarti RM, bukan pass seperti mode 1
  ketika ukuran sama. Tes C++ lama yang mengharapkan RN pada perubahan ukuran
  dikoreksi untuk memakai raw mode 0. Integer rounding equal-size tetap
  memakai mode 4/5/6/7.
- **56.240/56.240 kasus F2F cocok**: rounding RN/RM/RP/RZ, integer rounding,
  abs/neg/SAT/FTZ, H0/H1, NaN/infinity, subnormal, tie dan tetangganya, overflow,
  underflow, random, guard/inversion, serta cbuf/immediate dan overlap output
  dengan word source rendah/tinggi. Payload NaN hasil konversi dan tanda zero
  SAT dikecualikan dari perbandingan; instruksi yang tidak dieksekusi harus
  mempertahankan seluruh bit, termasuk payload NaN.
- Runner file ALU kini menerima header opsional `CBUF <hex words...>` untuk
  slot 2. Oracle F2I/I2F juga diperluas dengan **10.560 kasus** cbuf/immediate/
  overlap, sehingga **207.744/207.744 bit-exact**. Ini memperbarui batas operand
  dan overlap pada checkpoint sebelumnya; sumber F16/F64 F2I masih di luar
  oracle Python ini.
- Build app/runner berhasil (`-j 6`). Suite shader-library **58 grup PASS**
  dengan Metal validation. Tidak menjalankan game/GUI, commit, atau staging.

**`.CC` F2F/F2I/I2F masih OPEN.** Sumber primer yang diperiksa menjelaskan
konversi numerik dan daftar opcode, tetapi belum menentukan efek konversi SASS
Maxwell terhadap Z/S/C/O. Tidak dibuat implementasi atau oracle berdasarkan
dugaan. Dibutuhkan spesifikasi primer atau trace NVIDIA yang bisa direproduksi,
terutama untuk NaN, saturasi, dan hasil 64-bit; warning unsupported tetap ada.
Tahap konversi maupun jalur grafis keseluruhan belum dinyatakan selesai.

Reproduksi: [README F2F](../../tests/README-maxwell-f2f-reference.md) dan
[README konversi](../../tests/README-maxwell-conversion-reference.md).
Laporan lokal: `/tmp/hydra-f2f-before.json`, `/tmp/hydra-f2f-verified.json`,
`/tmp/hydra-conversion-forms.json`; log regresi:
`/tmp/hydra-f2f-shader-verified.log`.

## CTR: warning uniform, scene hitam, dan perbaikan rentang baca

Pengguna menjalankan build setelah konversi, melaporkan stuck dan intro hijau.
Inspeksi live memperlihatkan game masih mengirim draw; kemudian HUD race tampil
di atas scene 3D hitam. FPS present 60 tidak membuktikan gambar 3D benar.
Log awal tumbuh sampai sekitar 275 MB, didominasi `Uniform buffer at index 1
is not bound`. Sampel thread masih menunjukkan eksekusi PFIFO/draw, bukan bukti
deadlock. Intro hijau belum ditentukan penyebabnya secara terpisah.

**Penyebab warning terbukti:** fragment c1 menunjuk literal constants setelah
kode shader dalam alokasi kecil, tetapi selector mengiklankan batas 64 KiB.
Resolver lama memeriksa seluruh selector dan menolak binding saat mencapai
ekor tak terpetakan. Contoh live: shader `0x860030`, c1 `0x861600`, mapping valid
2.560 byte, sedangkan shader hanya membaca 24 byte. Python mengaudit dump MSL
dan log; bukti ada di
`/tmp/hydra-uniform-20261019-203600/uniform-evidence.json`. Frame trace sebelum
perbaikan memuat 130 laporan `FS c1: NOT BOUND`.

Perbaikan:

- MemoryAnalyzer menghitung prefix yang benar-benar dibaca tiap slot. Indeks
  unsigned yang dibatasi min/mask/shift/select dapat dibuktikan dalam satu
  blok; batas dibuang saat kondisi, perpindahan blok, overwrite tak diketahui,
  atau kemungkinan overflow. Konversi konstanta signed nonnegative ke unsigned
  juga dikenali (dibutuhkan jump table shader `0x7ef0030`). Indeks tanpa batas
  yang terbukti tetap meminta validasi seluruh binding.
- ResourceMapping membawa ukuran prefix ke binding uniform 3D. Resolver tetap
  memeriksa seluruh prefix, menggabungkan mapping terpisah, mempertahankan
  snapshot upload, dan menolak lubang maupun pembacaan melebihi ukuran selector.
  Tidak menambahkan data nol untuk menyamarkan mapping yang hilang.
- Konsumen CPU untuk branch table, texture handle, storage descriptor dan
  capture memakai rentang yang dibutuhkan. BRX tidak lagi kehilangan constant
  table hanya karena ekor selector tak terpetakan.
- Diagnostik kegagalan mencatat stage, slot guest, alamat/ukuran, shader dan
  jumlah kegagalan; log repetitif dibatasi, dengan
  `HYDRA_LOG_UNBOUND_UNIFORMS=1` untuk seluruh kejadian. Ini instrumentasi,
  bukan dasar klaim bahwa binding berhasil.

Build app, GPU engine dan shader-library berhasil (`-j 6`). Tes engine mengikat
selector 64 KiB ke mapping kecil lalu membaca prefix lewat GPU Metal; byte hasil
cocok. Prefix yang menyentuh lubang, permintaan melebihi selector dan indeks
tanpa batas tetap ditolak. Tes analyzer memeriksa direct/bounded-index reads,
conditional overwrite, clobber float, dan overflow. GPU engine serta **59 grup
shader PASS** dengan Metal validation. Log:
`/tmp/hydra-uniform-engine-verified.log` dan
`/tmp/hydra-uniform-shader-verified.log`.

Percobaan perbaikan awal menyisakan satu shader `0x7ef0030`; perbaikan terakhir
menangani cast konstanta dalam batas indeksnya. Build terakhir dibuka ulang di
`/tmp/hydra-uniform-verified-20261019-204709`, dengan statistik FPS dan diagnostik.
**Verifikasi ulang hilangnya warning, pemulihan scene 3D, dan intro hijau pada
build terakhir masih menunggu gameplay pengguna.** Agen tidak mengirim input
gameplay. Tidak commit atau staging; tahap grafis tetap OPEN.

### Verifikasi pengguna setelah perbaikan prefix

Pengguna menjawab **"sudah normal"** untuk pertanyaan apakah scene 3D sudah
tampil dan intro tidak lagi hijau. Ini menggantikan status menunggu verifikasi
di paragraf sebelumnya. Run terakhir
`Hydra_2026-10-19_13-47-33.045905.log` mencatat **0** `Uniform buffer unavailable`
dan **0** `Constant buffer mapping failure`, sampai akhir sesi sekitar 3 menit
9 detik. Tidak ada perubahan brightness, warna video, atau shader khusus CTR.

Sampel present sempat 59,8 FPS sebelum loading/race; sampel terakhir 44,6 FPS
dengan 728 draw/frame, CPU encode 22,40 ms/frame dan Metal GPU 13,24 ms/frame.
Ini scene berbeda, bukan perbandingan performa A/B. Keberhasilan gambar tidak
berarti performa stabil 60 FPS. App keluar setelah serialisasi user dan
penyimpanan cache shader; monitor merekam akhir sesi pada 20:50:49 WIB.
Data FPS ada di `/tmp/hydra-uniform-verified-20261019-204709/fps.csv`.


## Crash 4: cache pipeline berulang dan audit warning (21:10 WIB)

Sesi sebelum perbaikan: `/tmp/hydra-ctr-check-20261019-205325`, PID 62321,
log `Hydra_2026-10-19_13-53-32.074404.log`. Pengguna memilih
**Crash Bandicoot 4: It's About Time** (010073401175e000). Jendela awal 0 FPS,
lalu screenshot ingame menunjukkan 3 FPS. Sampling PFIFO memperlihatkan
pembuatan shader/pipeline tetap terjadi setelah scene tampil. Binary archive
mencatat 18.304 hit / 350 miss pada 00:05:44; hit berarti pembuatan pipeline dari
arsip disk, bukan hit cache objek pipeline di RAM. Footprint sampling 5,4 GiB;
swap sistem sekitar 2,6 GiB. Metal API Validation aktif pada sesi tersebut.
Proses sudah keluar sebelum attach debugger; tidak ada diagnosis crash dari
fakta ini. Flag `HYDRA_PERF_STATS` terlewat saat launch, sehingga statistik
CPU/GPU per frame tidak tersedia untuk baseline ini.

### Perbaikan cache

Snapshot shader sebelumnya selalu membandingkan sampai 64 KiB, termasuk data
sesudah instruksi yang didekode. Perubahan tail tersebut menciptakan identitas
shader baru dan pointer baru pada kunci pipeline, meski hasil MSL tetap sama.
Ini bug yang direproduksi tes; kontribusi tepatnya ke 3 FPS masih perlu uji game.

Decompiler sekarang mencatat prefix yang benar-benar dibaca, termasuk header,
scheduling words, dan blok kontrol alur yang dikunjungi. Cache menyimpan versi
kompilasi per state/alamat/address-space/generasi hot reload, membandingkan
prefix terhadap mapping terkini, dan memeriksa kata cbuf yang menjadi dependensi
branch. Objek shader/pipeline lama dipakai lagi bila dependensi cocok. Jalur
indirect/absolute jump dan decode keluar batas tetap memakai validasi jendela
penuh secara konservatif. Perubahan instruksi di dalam prefix, header, remap,
dan invalidasi GPU tetap dideteksi; snapshot lama tetap dimiliki cache.

`shader_cache_reference.py`: **184/184**, NVIDIA header audit 8 nilai, bit-exact
Python vs pixel Metal. Runner juga mengubah 64 byte tail sambil merender dan
memastikan pointer shader/pipeline tetap sama serta pixel identik. Cbuf JMP
berubah tanpa code write menghasilkan warna dan shader berbeda; target
A→B→A kembali memakai artifact A. Lookup hangat sekitar 2,8 us dalam fixture,
bukan klaim waktu game. Laporan: `/tmp/hydra-cache-prefix-reference-verified.json`.

### Semua jenis warning pada log sesi tersebut

| Warning | Penanganan |
| --- | --- |
| `Unknown request 12 for IResolver` (4 kali) | Command 12 kini mengembalikan respons resolver offline dengan ABI output benar: size, EAI_AGAIN, TRY_AGAIN, errno EAGAIN. Command 6 juga diperbaiki agar tidak mengembalikan sukses kosong. Ini penanganan kegagalan offline, bukan dukungan DNS/jaringan penuh. |
| `Invalid buffer address 0` | Diagnostik salah untuk wrapper NV IPC: payload characteristics ditulis inline/out-buffer, alamat guest tidak dipakai. Pointer nol dan size query tidak lagi dianggap error pointer. |
| `Channel method 0x14 not implemented` | Implementasi `NVB06F_SET_REFERENCE` (byte offset 0x50), nilai 32 bit per channel dipublikasikan setelah sinkronisasi pekerjaan GPU sebelumnya. |
| `unsupported stage geometry enabled` | Warning debug mendahului validator yang justru mengenali penerus layer (`0xac930`, atribut `0x90`). Validator geometry kini menjadi sumber diagnosis; geometry yang gagal dikenali tetap warning. Tidak mengklaim geometry shader umum didukung. |
| `Texture copy bgra8unorm → rg32float not implemented` | Cache tidak lagi melaporkan copy GPU berhasil untuk ukuran texel berbeda. Alias kembali melalui encode/decode byte guest yang sudah ada. Tes clear GPU/readback BGRA8↔RG32Float memeriksa semua 256 byte dua arah. |
| `Long wait ... result 0xec01` saat proses berakhir | `Cancelled` adalah hasil pembatalan yang diharapkan; tidak dilaporkan sebagai long-wait abnormal. Hasil wait dan mekanisme pembatalan tidak berubah. Warning untuk wait panjang lain tetap ada. |

Sumber: [NVIDIA clb06f.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/host/clb06f.h)
untuk SET_REFERENCE;
[libnx sfdnsres](https://github.com/switchbrew/libnx/blob/master/nx/source/services/sfdnsres.c)
untuk respons resolver lama;
[Atmosphere IResolver](https://github.com/Atmosphere-NX/Atmosphere/blob/master/stratosphere/ams_mitm/source/dns_mitm/dnsmitm_resolver_impl.hpp)
untuk ABI command 12 (implementasi IPC homebrew, bukan emulator pembanding).
Mapping format/alias ditangani lewat byte guest yang ada di engine, diuji dengan
Metal validation. Tidak mengubah warna, mematikan correctness, atau menghapus
warning untuk fitur yang benar-benar tidak didukung.

Validasi final dan sesi uji FPS setelah build dicatat di bawah. Semua perubahan
belum di-stage/commit. Peningkatan FPS game belum diklaim sebelum pengguna
menguji build baru.

Validasi akhir: build `hydra` dan lima binary tes berhasil; engine 34 grup,
shader 59 grup, tekstur 38 grup, IPC services 1 grup, semuanya exit 0 dengan
`MTL_DEBUG_LAYER=1`. Detail `/tmp/hydra-warnings-final-tests.json`.
Hydra dibuka kembali pukul 21:10:44 WIB (PID 73086), sesi
`/tmp/hydra-cache-warnings-fixed-20261019-211044`, `HYDRA_PERF_STATS=1`;
Metal API Validation tidak diaktifkan pada run pengukuran ini. Uji GPU tetap
menggunakan validation. GUI sudah berada di daftar game; game/input dipilih
pengguna. Statistik tersimpan ke `fps.csv`, `perf.log`, dan `latest.json`.
Peringatan scan library NSZ Teeto ditutup agar daftar game bisa digunakan;
dukungan kompresi NSZ bukan bagian perbaikan log runtime Crash 4 ini.

## Crash 4: FPS rendah. Checkpoint GPU, report occlusion asinkron, alias mip tail (22:00 WIB)

Semua angka "Python vs Metal" di bagian ini adalah model referensi vs eksekusi
GPU Apple, **bukan GPU NVIDIA fisik**.

### Bukti sebelum perbaikan

Sesi `/tmp/hydra-cache-warnings-fixed-20261019-211044` (log game
`Hydra_2026-10-19_14-11-25.053989.log`). Pengguna menutup app normal pukul
21:16:43 (serialisasi user dan simpan cache shader tercatat; tidak ada crash).

- 5,4 sampai 7,1 FPS, CPU encode 140 sampai 186 ms/frame, 210 command buffer,
  sekitar 4.300 draw, 261 render encoder dan 57 blit encoder per frame.
- Log `CHECKPOINT:` yang sudah ada: 2.000 sampai 2.900 checkpoint per 2 detik
  (sekitar 200 sampai 230 per frame), thread PFIFO terblokir 1.100 sampai
  1.400 ms per 2 detik (**sekitar 65 %** waktunya), rata-rata 0,5 ms per
  checkpoint, padahal GPU hanya aktif 35 %. Biaya sebuah checkpoint adalah
  round trip commit, jadwal, selesai, bukan lamanya kerja GPU.
- Sampling thread PFIFO (`sample-low-fps.txt`, 306 sampel): `setReportSemaphore →
  getOcclusionCounter → synchronize` 116 sampel (38 %); **`invalidateShaderCaches →
  synchronize` 75 sampel (24,5 %)**; makro draw 87 (28 %). Hambatan kedua tidak
  ada di handoff sebelumnya. Ia datang dari perbaikan cache shader hari ini:
  `INVALIDATE_SHADER_CACHES` (bit INSTRUCTION) memanggil checkpoint tanpa syarat,
  dan NVN memanggilnya puluhan kali per frame.

### Report occlusion asinkron

Report ZPASS (counter 0x2/0x15) tidak lagi menunggu GPU di thread PFIFO.

- `CompletionQueue` (`renderer/metal/completion_queue.*`): antrean berurutan
  dengan thread sendiri. Setiap pekerjaan menunggu command buffer Metal-nya
  (`CompletionToken`) selesai **dan** pekerjaan sebelumnya; urutan selesai Metal
  tidak diasumsikan. Thread ini terdaftar di debugger sehingga boleh mencatat
  log (thread completion Metal tidak boleh).
- `OcclusionCounter` (`renderer/metal/occlusion.*`): draw menulis hasil visibility
  ke halaman per encoder (4096 slot; tidak ada batas 128 halaman dan tidak ada
  checkpoint saat batas). Antrean memuat segmen halaman (menjumlah ke total
  berjalan), reset (mengosongkan total, memulihkan validitas) dan report
  (mempublikasikan total saat itu). Snapshot di titik report, epoch reset,
  report berulang dan counter yang dimatikan tetap seperti semantik lama.
  Command buffer yang gagal tidak mempublikasikan apa pun dan membuat counter
  tidak valid sampai reset berikutnya. Halaman kembali ke pool hanya setelah
  GPU selesai menulisnya.
- Engine (`ThreeD::queueCounterReport`): tujuan divalidasi dan tekstur GPU yang
  menimpanya di-flush saat report dikirim. Penulisan memetakan ulang alamat lewat
  GMMU saat GPU selesai (seperti GPU), menulis timestamp lalu nilai, dan
  invalidasi cache diantrikan (`IRenderer::queueMemoryInvalidation`, diterapkan
  PFIFO sebelum entry berikutnya) karena cache dikunci thread GPU selama draw.
  Address space ditahan `shared_ptr`; address space yang dibatalkan tidak
  ditulis; `Pfifo::cancel` menunggu antrean kosong sebelum memori dibebaskan.
- Callback logical submission (fence nvhost) dipindah ke `onCompletedChecked`
  dan dijalankan **setelah** report submission itu masuk memori. Submission tanpa
  report tetap memakai handler native langsung, tidak tertahan report lain.
  Checkpoint tidak menjalankan callback logical (tidak berubah).
- Konsumen CPU yang membaca/menulis memori tujuan report memanggil
  `IRenderer::flushReports(range)`: bila ada report yang masih menulis rentang itu,
  submission di-commit dan ditunggu. Titik panggil: `TextureCache::flushMemory`
  dan `discardGpuWrites` (mencakup DMA, 2D, inline-to-memory, payload release,
  reduction), render kondisional (`shouldExecute`), acquire semaphore 3D, dan
  semaphore puller.
- Belum asinkron (masih checkpoint): payload release dan reduction (2,4 sampai
  3,1 per frame di Crash 4, 2 sampai 6 ms/frame), `INCREMENT_SYNC_POINT`
  (0,6 sampai 1,6 per frame), semaphore puller, readback tekstur.

### Invalidasi shader tidak lagi menunggu draw biasa

`Renderer::synchronizeGuestWrites` hanya checkpoint bila sejak checkpoint
terakhir ada kerja GPU yang menulis memori guest (storage buffer yang
`written`, di draw maupun compute; mereka alias memori guest). Draw ke render
target tidak bisa menulis kode shader. Engine sudah men-checkpoint tepat setelah
tulisan storage (`synchronizeStorageWrites`), jadi di produksi jalur ini hampir
tidak pernah menunggu. Versi pertama saya menandai tiap encoder compute dan
terlalu konservatif: Crash 4 punya 9 dispatch compute per frame yang tidak
menulis storage dan tetap menyebabkan 9 checkpoint (3,6 ms) per frame, 21,6 %
waktu PFIFO pada profil sesi uji pertama. Kini hanya dispatch dengan binding
`written` yang menandai. Tes cache shader dengan kode ditulis GPU (action 11 dan
12) memodelkan kernel penulis dengan `noteGuestMemoryWrites()`.

### Statistik baru di `HYDRA_PERF_STATS=1`

`CHECKPOINTS waited N/frame (ms/frame) | alasan n (ms) ...` (alasan: shader
invalidate, syncpoint, report release/flush, conditional render, acquire, puller
semaphore, texture readback, storage write) dan `REPORTS: N queued/frame, M
forced a wait`. Alasan dipasang lewat `ScopedCheckpointReason` pada command
buffer yang menunggu; hitungan mencakup semua thread.

### Warning alias R8 (20 `|W|` di log sesi tadi)

Semua 20 warning sama: tekstur R8 mipmapped (256x256 dengan 9 level atau 512x512
dengan 10 level, block-linear) dan tekstur satu level R8 selebar 64 yang
alamatnya persis awal level 3 sampai 8 (atau 4 sampai 9) rantai tadi: 64x32,
64x16, 64x8, 64x4, 64x2, 64x1, masing-masing berukuran satu level rantai (0x800,
0x400, lalu 4 x 0x200; offset 0x15000, 0x15800, 0x15C00, 0x15E00, 0x16000,
0x16200 pada rantai 256x256 berukuran 0x16400). Game menulis level kecil rantai
sebagai tekstur sendiri selebar satu GOB (64 byte per baris). Jalur "layout
sama, dimensi beda" di `synchronize2DWith2D` hanya berjalan bila pointer dasar
kedua tekstur sama (kasus render target berpadding), jadi alias ini ditolak;
fallback lewat memori guest juga ditolak karena rantai punya tulisan GPU sendiri
(dekode ulang akan menimpanya). Hasilnya level kecil rantai (atau tekstur kecil
itu sendiri) basi dan muncul warning. Perbaikannya: syarat pointer sama dibuang
untuk format tak terkompresi; yang dipakai adalah awal wilayah bersama yang
jatuh tepat pada batas level di kedua sisi dan `haveCompatibleLayout` (block
size per level dan stride sama). Wilayah yang disalin adalah irisan dimensi
(32x32 dari 64x32, kolom padding tidak disentuh), layer hanya disalin banyak bila
`layer_size` sama. Format terkompresi tetap lewat jalur guest karena regionnya
harus selaras blok.

Tes baru `texture_cache_tests`: rantai R8 256x256 dengan tulisan GPU sendiri,
enam tekstur ekor menurut log, arah ekor ke rantai (semua piksel setiap level
cocok) dan rantai ke ekor (kolom padding tetap). Mutasi (syarat pointer sama
dikembalikan) membuat tes gagal dan mencetak warning yang identik dengan log
game ("DEBUG cannot synchronize 2D textures ... r8unorm, 256x256, level count 9").

### Validasi (build dengan `-j 6`, semua dengan `MTL_DEBUG_LAYER=1`)

- gpu-engine 38 grup, shader-library 59, texture-cache 39, raster-state,
  render-hash, register-write, frame-trace, visual-diagnostic, buffer-snapshot:
  semua exit 0. Cek integrasi shutdown (13 selector) dan shutdown penuh exit 0.
- `shader_cache_reference.py` 184/184 (termasuk kode ditulis GPU + invalidasi),
  8 nilai header NVIDIA diaudit.
- `tests/occlusion_reference.py` (README-occlusion-reference.md): 300/300 program
  acak cocok bit demi bit; kontrol yang sengaja salah gagal di 97.
- Tes baru gpu-engine: report tidak menunggu GPU dan tidak terbit sebelum commit;
  callback submission berjalan setelah report terbit (juga dengan publisher
  lambat 150 ms); submission tanpa report tidak tertahan report lambat; urutan
  report/reset berantai; memori terpecah; alamat di-unmap atau di-remap sebelum
  terbit; address space dibatalkan; command buffer gagal (token disuntik) tidak
  mempublikasikan dan counter tetap invalid sampai reset; render kondisional dan
  acquire menunggu report yang tertunda; release sesudah report ke alamat yang
  sama menang; `flushMemory` menunggu report; invalidasi shader hanya menunggu
  tulisan guest; antrean penyelesaian berurutan walau token selesai terbalik.
- Mutasi yang terbukti terdeteksi: `flushReports` kosong, callback tidak dirantai,
  kegagalan tetap dipublikasikan (dua tingkat), hook render kondisional,
  hook acquire, hook `flushMemory`, invalidasi selalu checkpoint, invalidasi
  tidak pernah menunggu (8 dari 184 kasus cache gagal). 25 sampai 40 kali ulang
  suite engine tanpa kegagalan. Satu crash laporan (21:58, `hydra-gpu-engine-tests`)
  berasal dari tes yang gagal oleh mutasi dan meninggalkan report tertunda saat
  GMMU lokalnya hancur; fixture kini menguras report di destruktor, dan mutasi
  yang sama sekarang gagal bersih tanpa crash.

### Hasil di game (sesi uji pertama `/tmp/hydra-async-reports-20261019-220244`)

Build tengah (invalidasi masih konservatif). Report 160 sampai 205 per frame,
**0 yang memaksa menunggu**; checkpoint menunggu turun dari sekitar 200 menjadi
12 sampai 66 per frame; command buffer 210 menjadi 17 sampai 75 per frame;
profil PFIFO (`sample-after-fix-10s.txt`): jalur occlusion **0 %**, invalidate
21,6 %, readback copy DMA 4,8 %, makro draw 70 %. Saat mesin tenang 9,6 sampai
11,1 FPS (sebelumnya 5 sampai 7). **Pengukuran sesi itu tidak bersih**: build
dan tes saya berjalan bersamaan (load average 26, swap 3,7 GB), dan FPS jatuh
ke 2 sampai 5 tiap kali itu terjadi. Jangan memakai dip itu sebagai hasil kode.

### Batas dan berikutnya

- Build akhir (invalidasi sempit) dibuka ulang pukul 22:14 (sesi
  `/tmp/hydra-async-reports-final-20261019-221421`); hasil gameplaynya belum
  tercatat di sini.
- Sisa waktu thread PFIFO terutama kerja CPU draw: `getPipeline` 17 %
  (`ShaderCache::find` ulang validasi prefix kode tiap lookup setelah
  invalidasi, `memcmp`), `configureShaderStage` 14 %, `bindDrawState` 6 %, dengan
  sekitar 4.300 sampai 5.100 draw per frame. Itu kandidat berikutnya
  (memoisasi shader/pipeline, tracking tulis kode).
- Readback tekstur dari engine copy (4,8 %) dan semaphore release/syncpoint masih
  sinkron.
- Timestamp report diambil saat publikasi (setelah GPU selesai), bukan saat GPU
  mencapai titik report; sama dengan perilaku lama.
- Semantik report di memori (nilai lalu timestamp, mode kondisional EQ/NE) tetap
  belum diverifikasi terhadap hardware NVIDIA; lihat dokumen occlusion.


## Warning alias terbaru: GPU-owned destination (22:32 WIB)

Log pengguna `Hydra_2026-10-19_15-15-12.183470.log` memuat 20 warning
`DEBUG cannot synchronize 2D textures` yang terlihat pada 00:01:12–00:01:22.
Pasangan meliputi rantai R8 256/512 dengan `block_width_gobs_log2=2` vs target
single-mip dengan nilai 0, serta RG32Float/RGBA32Float dan R8/RGBA32Float yang
ukuran texelnya berbeda. Jalur copy langsung secara konservatif menolak layout
tersebut; fallback guest sebelumnya juga menolak destination yang masih punya
GPU writes sendiri. Hasilnya sinkronisasi tidak terjadi.

Perbaikan terfokus di `texture_cache_hot.cpp`: fallback mengumpulkan semua
storage GPU yang overlap dengan rentang destination, termasuk destination,
memutar ulang byte hasil GPU berdasarkan urutan timestamp tulisan, lalu decode
destination satu kali. Replay mencakup alias baru yang sudah di-flush; jika
alias itu dilewati, destination lama bisa menimpa byte yang lebih baru.
`stale_ranges` tetap mengecualikan tulisan CPU; retile yang ada tetap menjaga
padding. Setelah replay seluruh overlap, loop copy langsung dihentikan agar
source yang lebih lama tidak menimpa hasil merge terbaru. Warning tidak dihapus.
Jalur fallback ini memakai readback sinkron; bukan klaim optimasi FPS.

Tes GPU baru menguji rantai R8 256×256/9 mip dan pasangan RG32Float64×64 /
RGBA32Float32×32 beserta offset source kedua. Destination diberi data GPU,
source di-update lalu di-flush lebih dulu. Seluruh byte texel semua mip diperiksa
terhadap model byte guest berdasarkan layout. Kontrol dengan penolakan lama
dikembalikan: exit1, dua warning yang identik dengan R8 log game dan mismatch
pixel. Kontrol sudah dihapus kembali. Log:
`/tmp/hydra-alias-replay-control-tests.log`.

Build app dan texture-cache sukses. Suite tekstur **40 PASS**, **0 warning
cannot synchronize**, Metal validation aktif:
`/tmp/hydra-alias-replay-final-texture-cache.log`.
`git diff --check` bersih. Tidak stage/commit. Gameplay build baru belum diuji.

Catatan integrasi tree saat ini: enum checkpoint sudah memakai `PullerOther`,
tetapi satu callsite GPU masih `PullerSemaphore` yang tidak ada; callsite
namanya diselaraskan supaya build berhasil, tanpa mengganti perilaku sinkronisasi.
Suite engine lebih luas berhenti di assertion lama
`A semaphore payload is published only after preceding GPU writes finish`
(`/tmp/hydra-alias-replay-final-gpu-engine.log`, 12 PASS sebelum gagal).
Kode release semaphore di tree sekarang memakai `queueGuestWrite` asinkron,
sementara assertion membaca nilai segera sebelum submit. Ini tidak diubah
oleh fix alias ini; perlu rekonsiliasi dengan pekerjaan occlusion/payload yang
sedang dilanjutkan di workspace. Jangan mengklaim seluruh regresi engine lulus.

Tambahan verifikasi: shader-library 59 PASS, exit0 dengan Metal validation;
`/tmp/hydra-alias-replay-final-shader.log`.


### Rekonsiliasi semaphore/syncpoint asinkron setelah konfirmasi pengguna

Pengguna memastikan tidak ada Claude lain yang sedang mengedit, dan meminta
perbaikan diteruskan. Tes release, reduction, dan increment syncpoint yang masih
membaca hasil segera kini memeriksa dua fase: sebelum submit hasil tetap lama
(tidak ada checkpoint yang dipaksa), lalu consumer `flushReports` atau
synchronize+drain menunggu publikasi sebelum memeriksa hasil. Pemeriksaan
witness GPU, urutan report sebelumnya, split mapping, timestamp, dan callback
logical submission dipertahankan. Tes tidak mengembalikan jalur produksi ke
sinkron per report.

Tes juga menemukan bug produksi tambahan: `OcclusionCounter::queueReport`
tanpa daftar rentang tulis tidak ditandai `ordered`; `drainReports` pada
WAIT_FOR_IDLE bisa melewati callback publikasinya. Semua report publik kini
ditandai ordered, termasuk callback tanpa rentang; pekerjaan akumulasi internal
bukan report tetap memakai mekanisme sebelumnya. Sebelum perbaikan, tes
`A wait for idle includes the reports queued ahead of it` gagal; setelahnya
suite engine penuh exit0. Ini menyelesaikan kegagalan engine yang dicatat di
bagian sebelumnya.

Validasi: engine 39 PASS dengan `MTL_DEBUG_LAYER=1`
(`/tmp/hydra-semaphore-async-engine.log`); Python–Metal occlusion 300/300
(`/tmp/hydra-semaphore-async-reference.json`). Build app dan engine berhasil
`-j 6`; diff check bersih. Tidak stage/commit. Uji gameplay build gabungan
masih pending pengguna.

### Lanjutan tahap A: puller semaphore unsigned INC/DEC

Permintaan pengguna: lanjut implementasi dan wajib membandingkan simulasi
Python NVIDIA dengan Metal sungguhan. Puller REDUCTION kini mendukung unsigned
INC dan DEC: INC mengembalikan nol jika nilai mencapai/melebihi payload;
DEC memuat payload jika nilai nol atau melebihi payload, selain itu mengurangi
satu. Jalur atomic compare/exchange, WFI dan invalidasi tetap digunakan.
Format signed INC/DEC serta opcode di luar 0–7 ditolak sebelum menulis memori
dengan fault Hydra 25. Angka fault ini kebijakan emulator, bukan enum NVIDIA
yang telah dibuktikan.

Bukti sumber dibatasi dengan jelas: `clb06f.h` Maxwell membuktikan encoding,
sedangkan tabel rinci persamaan/format berasal dari manual PBDMA Turing NVIDIA;
persamaan numerik juga cocok dengan PTX atom INC/DEC. Ini belum sertifikasi
seluruh perilaku GM20B. Sumber dan cara mengulang tes ada di
[`tests/README-semaphore-reduction-reference.md`](../../tests/README-semaphore-reduction-reference.md).

Runner Python baru memakai model matematis independen, mengaudit 12 definisi
header serta persamaan manual, dan menyimpan hash SHA-256 sumber. Harness C++
menjalankan puller produksi, dengan kernel Metal nyata menginisialisasi dan
membaca buffer shared beserta tujuh word tetangga. Operasi reduction produksi
tetap atomic CPU; ini bukan klaim arithmetic reduction berjalan dalam shader
Metal. Grup WFI menunggu GPU melalui jalur produksi; grup no-WFI menyelesaikan
inisialisasi sebelum pengukuran dan memastikan reduction tidak memaksa
checkpoint. Atomics CPU/GPU serentak, timestamp dan semua modifier belum diuji.

Validasi final: build `hydra` + engine sukses `-j 6`; suite engine **39 PASS**;
Python–Metal **3.272/3.272** (boundary signed/unsigned, seluruh delapan operasi,
WFI/no-WFI, random deterministik, rantai wrap, opcode invalid), Metal validation
aktif. Kontrol negatif yang sengaja merusak model INC gagal seperti diharapkan,
sementara runner Metal selesai normal. Log/report:
`/tmp/hydra-puller-reduction-final-build.log`,
`/tmp/hydra-puller-reduction-engine.log`,
`/tmp/hydra-puller-reduction-reference.json`,
`/tmp/hydra-puller-reduction-control.json`.

Tahap A tetap **OPEN**: END_SUBDEVICE_MASK, skip_buffer_refcounting, detail
AS_GPU, DeviceShared, lifetime SharedMemory, FROM_ID per-klien, channel recovery
dan validasi gameplay/hardware masih punya batas atau pekerjaan tersisa.
Belum ada hasil gameplay/FPS untuk perubahan ini. Tidak stage/commit.

### Fault channel: opcode yang tidak didukung tidak boleh diteruskan

Audit lanjutan tahap A menemukan jalur yang hanya mencatat method lalu
melanjutkan stream: opcode MEM_OP_D di luar enam opcode yang ditangani, default
puller method (termasuk ILLEGAL), dan method engine pada subchannel tanpa
binding. SET_OBJECT B06F juga hanya mencatat stub dan mengganti binding dengan
null. Semua jalur tersebut kini mem-fault decode state dengan kebijakan Hydra
25; unsupported rebind mempertahankan binding sebelumnya dan berhenti.
Fitur yang ditolak tetap belum diimplementasikan, termasuk CRC/YIELD.

Referensi primer `NVIDIA/open-gpu-doc/classes/host/clb06f.h` diperiksa ulang
untuk encoding MEM_OP_D, enam opcode yang dikenal, ILLEGAL dan SET_OBJECT.
Klasifikasi fault 25 adalah kebijakan aman emulator, bukan bukti detail fault
hardware untuk setiap opcode. Lima opcode TLB/L2 yang sebelumnya diterima
tidak diubah; efek cache dan operand lengkapnya belum disertifikasi.

Tes Python baru mengaudit sembilan konstanta dan dua bit field, lalu
membandingkan model klasifikasi independen dengan dispatcher produksi:
**352/352 PASS**. Mencakup 32 opcode memory-operation × lima operand high,
method unsupported, engine tanpa binding dan binding B06F. Tiap kasus
menjalankan blit Metal nyata 4 KiB dan publisher completion terurut; MEMBAR
harus menunggu keduanya. Diperiksa fault, jumlah checkpoint, visibilitas
publisher/word GPU, dan seluruh byte setelah drain. Kontrol negatif dengan
ekspektasi silent acceptance gagal seperti diharapkan; runner Metal exit0.
Ini bukan eksekusi NVIDIA fisik atau pembuktian seluruh semantik cache.

Tes antrean PFIFO juga menempatkan binding/write valid setelah setiap kategori
fault: tail tidak dieksekusi dan callback sukses tidak diterbitkan. Validasi
Metal aktif: engine **39 PASS**, selector channel-faults, pfifo-decode dan
channel-scheduling semuanya exit0. Build app/engine/shutdown sukses `-j 6`;
linker tetap melaporkan dependency Homebrew fmt/SDL dibangun untuk macOS lebih
baru dari deployment target. Tidak ada perubahan dependency pada sesi ini.

Cara ulang dan batas bukti:
[`tests/README-channel-method-reference.md`](../../tests/README-channel-method-reference.md).
Log/report `/tmp/hydra-channel-policy-build.log`,
`/tmp/hydra-channel-policy-shutdown-build.log`,
`/tmp/hydra-channel-policy-engine.log`,
`/tmp/hydra-channel-policy-faults.log`,
`/tmp/hydra-channel-policy-decode.log`,
`/tmp/hydra-channel-policy-scheduling.log`,
`/tmp/hydra-channel-method-reference.json`,
`/tmp/hydra-channel-method-control.json`.
Tahap A tetap OPEN; belum ada uji gameplay/FPS. Tidak stage/commit.

### Lifetime SharedMemory dan teardown page table

Permintaan pengguna setelah penjelasan prioritas berikutnya: langsung
implementasikan perlindungan SharedMemory ketika handle/mapping ditutup
sementara GPU masih memakai backing-nya.

Bug awal: `SharedMemory::~SharedMemory` langsung menghapus `IMemory`, sedangkan
`mapToRange` hanya memasang pointer CPU tanpa retain object. Mapping GPU juga
menyimpan pointer host. Kini Process mencatat setiap mapping shared dan
menahan referensi object; unmap harus cocok object, address, dan ukuran.
Map berukuran berbeda dari backing, tidak aligned, melewati batas address
space Hydra, atau menimpa region yang terisi ditolak. Handle null ditolak
sebelum logging mengakses object. Ini bukan klaim seluruh hasil/error dan
permission Horizon telah diverifikasi.

Allocation SharedMemory memakai shared ownership terpisah dari object. Tiap
proses menahan allocation yang pernah berhasil di-map hingga cleanup menutup
akses baru, membatalkan semua AS/PFIFO dan drain GPU. Setelah itu mapping refs
dan allocation pins dilepas. CPU unmap tidak melepas pin backing karena
mapping GPU atau pointer Metal lama masih bisa memakainya. Kebijakan ini
konservatif seperti retirement heap: alokasi dapat menambah penggunaan RAM
hingga proses berakhir; early reclamation belum diimplementasikan. Tidak ada
pekerjaan/refcount baru per draw.

Tes berulang menemukan bug tambahan yang menghentikan runner dengan SIGABRT:
PageAllocator Hypervisor membebaskan host blocks tanpa `hv_vm_unmap`, lalu
proses berikutnya memakai slot physical page table yang sama dan `hv_vm_map`
gagal. Allocation sekarang menyimpan PA masing-masing block dan unmap VM
sebelum membebaskan host block. Bukti kegagalan sebelumnya ada pada crash
report `~/Library/Logs/DiagnosticReports/hydra-shutdown-tests-2026-10-19-230011.ips`
(stack PageAllocator::allocate → Mmu::Mmu → Process::Process). Setelah fix,
200 program dengan pembuatan/penghancuran proses berulang selesai normal.

Model Python independen memeriksa **200 program / 10.064 langkah**, dua
proses/object/slot mapping: close/transfer handle, map ganda, unmap object atau
ukuran salah, map invalid, dan cleanup proses. Tiap program menahan pembacaan
blit Metal nyata 16 KiB dari buffer no-copy melalui GMMU, melakukan mutasi
handle/mapping, lalu membandingkan seluruh byte. Diamati juga destructor
object, pemilik allocation dan mapping CPU. Fixture memiliki guard agar tes
rusak aman dibersihkan; guard dikurangi dari hitungan ownership, jadi hasil
GPU benar saja tidak dapat menyembunyikan pin produksi yang hilang.

Selector cleanup terpisah menghancurkan proses secara asinkron saat GPU
ditahan, baik dengan CPU mapping aktif maupun sudah di-unmap/handle terakhir
ditutup. Cleanup wajib menunggu GPU dan mempertahankan backing selama wait;
object/allocation harus dilepas setelah GPU selesai dan guard dibuang.

Validasi Metal aktif: Python **10.064/10.064 PASS**; kontrol negatif yang
mengabaikan retirement mendeteksi **348 mismatch** (runner Metal exit0);
selector shared-memory-lifetime, gpu-heap-resize, gpu-as-lifecycle dan
gpu-process-cancel exit0; suite engine **39 PASS**; default repeated System
teardown exit0. Build app, engine dan shutdown sukses `-j 6`; warning linker
dependency fmt/SDL target macOS lama masih sama. Diff check bersih.

Referensi/batas: libnx sebagai kontrak caller, Switchbrew sebagai deskripsi
reverse-engineered mapping refs, Apple Metal/Hypervisor docs dan SDK sebagai
otoritas host. Ini tes lifecycle host, bukan simulasi instruksi NVIDIA atau
eksekusi NVIDIA fisik. Balapan lookup/close handle table, seluruh API alias CPU,
permission/error lengkap dan reclaim lebih awal tetap belum disertifikasi.
Detail ulang: [`tests/README-shared-memory-lifetime.md`](../../tests/README-shared-memory-lifetime.md).
Report/log `/tmp/hydra-shared-memory-reference.json`,
`/tmp/hydra-shared-memory-control.json`,
`/tmp/hydra-shared-memory-cleanup.log`,
`/tmp/hydra-shared-memory-shutdown.log`,
`/tmp/hydra-shared-memory-engine.log`,
`/tmp/hydra-shared-memory-final-build.log`.
Tahap A tetap OPEN; belum diuji gameplay/FPS. Tidak stage/commit.
