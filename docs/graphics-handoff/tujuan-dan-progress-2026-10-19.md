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
- Belum: FSETP/ISETP, konversi F2I/I2F, TLD/TLD4/TXD, FMZ, mode rounding
  non-RN, instruksi half/double lewat jalur Python.

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
