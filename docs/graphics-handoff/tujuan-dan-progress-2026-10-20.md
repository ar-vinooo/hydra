# Progres 20 Oktober 2026

## Tahap A: ditutup dengan batas (19-20 Oktober)

Heap `resizeHeap` kini satu reservasi host 4 GiB yang di-resize di tempat (tidak
ada salinan, tidak ada tulisan hilang); MapBufferEx+Modify divalidasi sebagai
update kind (NVN memakai handle null); FROM_ID lintas proses tetap NotSupported.
Detail dan batas: [stage-a-exit-audit-2026-10-19.md](stage-a-exit-audit-2026-10-19.md).
Terbuka dan tidak bisa dibuktikan tanpa trace: kontrak Switch (FREE, error,
VA pool), END_SUBDEVICE_MASK, gameplay/hardware. Crash 4 masuk game setelah
perbaikan Modify.

## Tahap B: nilai format TIC (baru)

Sebelumnya hanya struktur (enum → format Metal) yang diuji. Kini
`tests/texture_format_reference.py` + `hydra-texture-format-reference-tests`
membandingkan byte tekstur → nilai terbaca Metal dengan dekoder Python:
**10.289.296/10.289.296 cocok**: 19 format tak terkompresi dan BC1-BC5, BC6H, BC7 (186.752 nilai; dekoder Python untuk BC1-5, `bcdec` untuk BC6H/BC7, diunduh saat tes), serta layout block-linear (2.058.960 nilai, GOB diimplementasikan ulang di Python; R8/GR8/ARGB8/RGBA16/RGBA32 byte-exact dan DXT1/DXT45, block height 1-16), serta mip chain dan array layer (8.043.584 nilai, 3-8 level, 1 atau 3 layer; konvensi pengecilan block height dan padding layer cocok dengan Hydra). Bug nyata: alpha
`XBGR8` terbaca sebagai byte X; diperbaiki di `toSampledSwizzleChannels`.
Dokumen: [README-texture-format-reference.md](../../tests/README-texture-format-reference.md).
Belum: ETC/EAC/ASTC, depth/stencil, 3D, cube, sampling terfilter. Block height lebih besar dari kebutuhan tinggi gambar dikecilkan Hydra (konvensi mip), tidak diuji.
Regresi: texture-cache 40, gpu-engine 39, shader-library 59, raster-state,
render-hash lulus. Belum diuji di game. Tidak stage/commit.

## Tahap B: engine DMA copy (baru)

`tests/copy_engine_reference.py` + `hydra-copy-engine-reference-tests`: model
Python dari engine copy (B0B5) lawan engine Hydra lewat antarmuka method, 592
kasus acak, 45 offset/field register diaudit terhadap `clb0b5.h`. Pitch ke pitch
(plain dan remap), semaphore, dan salinan tiled (pitch, block-linear) lulus.
**Bug nyata:** remap komponen umum (pemilihan komponen, konstanta, jumlah komponen
berbeda) pada salinan tiled diabaikan Hydra (hanya `ASSERT_DEBUG`, jadi build
release menyalin byte biasa); game yang mengisi tekstur block-linear dengan
konstanta lewat DMA akan salah tanpa peringatan. 120 kasus gagal sebelumnya;
`Copy::executeCopy` kini me-remap elemen demi elemen dan semuanya lulus.
Kontrol: menukar konstanta di model menggagalkan 95 dari 150 kasus remap.
Semantik remap pada salinan tiled (satuan elemen, lebar dalam elemen) adalah
konvensi driver, bukan teks NVIDIA; lihat
[README-copy-engine-reference.md](../../tests/README-copy-engine-reference.md).
Belum: blit 2D dan inline-to-memory. Regresi: dma-slice, gpu-engine 39,
texture-cache 40 lulus. Belum diuji di game.

## Tahap B: blit 2D dan inline-to-memory (baru)

`tests/blit_2d_reference.py` (500 kasus: blit GPU Metal SRCCOPY pitch/tiled,
skala 1/2/0.5/-1/-2/0.25/1.5/0.75, clip; jalur CPU ROP dan volume) dan
`tests/inline_to_memory_reference.py` (200 kasus, tujuan pitch dan block-linear):
model Python lawan engine Hydra, register diaudit terhadap `cl902d.h`/`cla140.h`.
**Bug nyata:** blit 2D nearest dengan downscale faktor bulat menjatuhkan titik
sampel tepat di batas texel; koordinat float ternormalisasi di shader Metal
kadang memilih texel bawah (sekitar 40% kasus, 120 dari 300). Sumber sampel
nearest kini digeser 1/512 texel di `Renderer::blitTexture`. Juga: `GMmu::getMmu()`
dianggap selalu ada di texture cache; sekarang dicek null. Semua lulus setelah
perbaikan. Kontrol: model yang memakai sudut piksel gagal; inline off-by-one
baris gagal. Detail dan batas:
[README-blit-2d-and-inline-reference.md](../../tests/README-blit-2d-and-inline-reference.md).
Dengan ini butir 3 tahap B (DMA, 2D, inline) selesai untuk yang bisa dibuktikan
dengan model; sampling terfilter, ORIGIN_CORNER, color key, dan blend 2D belum.
Regresi gpu-engine 39, texture-cache 40, shader-library 59, raster-state,
render-hash, dma-slice, frame-trace lulus. Belum diuji di game.

## Tahap B: sampler TSC (baru)

`tests/sampler_reference.py` (3000 sampel, 500 konfigurasi TSC acak): TSC melewati
`decodeSampler` + `specializeShaderSampler` + sampler Metal, shader TEXS 2D.LL
mengambil sampel tekstur bermip, dibandingkan dengan model Python. Lulus untuk
semua 8 mode alamat, filter min/mag, mip none/point/linear, clamp LOD, dan warna
border kustom (emulasi shader). Dua temuan, keduanya dicatat, belum diubah:
(1) bias LOD sampler tidak ditambahkan pada LOD eksplisit (TEXS.LL); header tidak
menjelaskan perilaku hardware, pertanyaan terbuka; (2) MIRROR_ONCE_BORDER dengan
filter linear: tap di indeks -1 dekat sambungan cermin membaca warna border, bukan
texel 0 (44 sampel; mode sangat jarang). Detail:
[README-sampler-reference.md](../../tests/README-sampler-reference.md).

## Tahap B: tipe tekstur 1D, array, 3D, cube (baru)

`tests/texture_type_reference.py`: 2400 sampel (600 per tipe) lewat jalur TSC +
TEXS yang sama dengan tes sampler, model Python lawan Hydra. Semua lulus.
**Bug nyata:** `TEXS 1D.LZ` menghasilkan MSL yang tidak bisa dikompilasi
(koordinat `vec<f32,1>` ke `texture1d::sample` yang butuh `float`); shader apa pun
yang men-sample tekstur 1D gagal dibangun. Diperbaiki di `emitTextureSample`
(`(coords).x`). Kontrol: membalik satu sumbu face +Y cube menggagalkan 98 sampel.
Detail: [README-texture-type-reference.md](../../tests/README-texture-type-reference.md).

## Tahap B: dekode primitif dan index buffer (baru)

`tests/index_decode_reference.py` (2500 kasus): semua tipe primitif (point, line,
line loop/strip, triangle, strip, fan, quad, quad strip, polygon) dengan index
8/16/32-bit atau tanpa index, restart mati, pada nilai kecil, atau pada nilai
maksimum; model Python dekomposisi standar lawan `IndexCache::decode`. Semua
cocok, tidak ada bug. Kontrol: membalik winding fan menggagalkan kasus fan dan
polygon. Detail:
[README-index-decode-reference.md](../../tests/README-index-decode-reference.md).
Belum: adjacency/patch, base vertex dan instancing.

## Tahap B: ASTC, ETC2, EAC (baru)

Tes format menambah blok ASTC void-extent (14 ukuran blok, Unorm/sRGB), ETC2 RGB/RGBA
(subset ETC1) dan EAC R11: 58.416 nilai cocok, tanpa bug. Memverifikasi pemetaan
ID TIC → ukuran blok → format Metal dan urutan blok. Kontrol: ASTC 5x4 yang
dimodelkan 4x5 menggagalkan tes. Belum: blok yang berinterpolasi, ASTC HDR, mode
T/H/planar ETC2, punch-through.

## Tahap B: viewport, scissor, window origin (baru)

`tests/viewport_reference.py` (500 konfigurasi, 75 titik NDC masing-masing):
register viewport/swizzle/window origin/clip Z/scissor ditulis ke engine 3D,
`getViewport`/`getScissor` membangun state Metal, dibandingkan dengan transformasi
viewport NVIDIA di model Python (posisi x, baris dari atas, kedalaman). Semua
cocok, tidak ada bug. Kontrol: model yang mengabaikan origin lower-left
menggagalkan setiap kasus lower-left. Perubahan kode produksi: hanya
`friend struct ThreeDTestAccess` di `ThreeD`. Detail:
[README-viewport-reference.md](../../tests/README-viewport-reference.md).

## Tahap B: status penutupan (20 Oktober 2026)

Semua oracle dijalankan satu perintah: `bash tests/run_python_references.sh`
(`ALL PASS`, sekitar 2,7 menit). Cakupan tahap B yang kini punya model Python
lawan Hydra: format TIC tak terkompresi, BC1-7, ASTC/ETC2/EAC, layout
block-linear, mip dan array; sampler TSC; tipe tekstur 1D/array/3D/cube; DMA copy,
blit 2D, inline-to-memory; dekode primitif dan index; viewport/scissor/window
origin; plus oracle lama (blend, vertex, depth/stencil, format RT, ALU).

Bug nyata yang ditemukan dan diperbaiki pada tahap B: alpha XBGR8 saat sampling,
remap komponen DMA pada salinan tiled, sampling nearest 2D di batas texel,
shader tekstur 1D yang tidak bisa dikompilasi, dereferensi MMU null di texture
cache. Pertanyaan terbuka yang dicatat (butuh bukti hardware): bias sampler pada
LOD eksplisit; emulasi MIRROR_ONCE_BORDER di sambungan cermin; semantik remap DMA
tiled dan blit 2D yang berupa konvensi driver.

Belum, dengan alasan: tekstur depth/stencil sebagai sampel (nilai, bukan hanya
layout; ada tes bentuk), dispatch compute (butuh shader dengan akses global),
blit 2D ORIGIN_CORNER/bilinear/color key/blend, reduksi semaphore DMA, blending ke
format RT 0x40-0x47, culling/winding (konvensi ditentukan empiris dari dua game),
adjacency/patch, dan MSAA/alpha-to-coverage (belum diimplementasikan, menunggu data
`HYDRA_PERF_STATS=1`). Uniform buffer dari backing tak-contiguous ternyata sudah
digather (`gathered_const_buffers`); catatan 18 Oktober usang. Gameplay: belum
diuji; prop hitam Crash 4 butuh capture frame.

## Sampler: bias LOD pada LOD eksplisit (diperbaiki)

Dari spesifikasi Vulkan (bagian "LOD Operation": lambda' = lambda_base +
clamp(sampler.bias + shaderOp.bias), lambda_base = LOD eksplisit bila ada) dan
aturan `textureLod` OpenGL, bias sampler ikut ditambahkan pada LOD eksplisit.
Hydra tidak melakukannya; `emitTextureSample` kini menambahkannya pada
`level(...)` (bukan `LZ`, tidak ada sumber). Bukti dari spesifikasi API, bukan
trace hardware Switch. Dampak visual di game (Crash 4 memakai bias -1 untuk dynamic
resolution) belum diuji. Pertanyaan terbuka tersisa: MIRROR_ONCE_BORDER di
sambungan cermin dan satuan remap DMA tiled (butuh source NVK/hardware).

## Tahap C: instruksi integer dan set-nilai (baru)

Oracle ALU (`tests/maxwell_alu_reference.py`) bertambah menjadi 12.930 kasus, semua
cocok, tanpa bug: **XMAD** (1536 kasus langsung: selektor H0/H1, bertanda/tidak,
mode C penuh/low/high/CBCC, PSL, MRG; dan 320 urutan yang dipakai kompiler untuk
perkalian 32-bit dan multiply-add, `XMAD`, `XMAD.MRG`, `XMAD.PSL.CBCC`, yang
harus sama dengan a*b(+c) mod 2^32), **BFE/BFI** (posisi dan panjang valid;
kasus tepi tidak dibandingkan karena semantik SASS belum tentu sama dengan PTX,
`.BREV` tidak diuji), **SEL**, **ISET** (8 operasi, bertanda/tidak, hasil -1
atau 1.0, kombinasi AND/OR/XOR dengan predikat), dan **FSET**. Semantik mode
CSFU, X, dan CC pada XMAD tidak dibandingkan.

## Tahap C: half precision (baru)

Oracle ALU bertambah ke 14.430 kasus: **HADD2/HMUL2/HFMA2** (1500 kasus acak;
model fp16 eksak dengan pembulatan RN-even; swizzle input F16/F32/H0H0/H1H1,
neg/abs, saturasi, output F16/MrgH0/MrgH1 dengan sisa setengah tujuan; nilai
khusus: nol bertanda, denormal, max, infinity, NaN) semuanya cocok. Kontrol: HFMA2
yang dimodelkan sebagai mul+add dibulatkan terpisah menggagalkan sejumlah kasus.
Belum: output F32 (maknanya tidak terdefinisi di sumber), bentuk C/I/32I,
FMZ, FTZ, HSET2/HSETP2.

## Tahap C: texel fetch TLDS (baru)

`tests/texel_fetch_reference.py` (2880 fetch): TLDS 1D.LZ/1D.LL, 2D.LZ, 2D.LL (rantai
mip), 3D.LZ dan 2D array LZ terhadap lookup array Python; semua cocok.
**Bug nyata:** `TLDS 1D.LL` menghasilkan `texture1d::read(coord, lod)` dengan LOD
dari register, padahal Metal mensyaratkan LOD konstan pada tekstur 1D; shader yang
memakainya gagal dibangun. Emitter kini memakai level 0 (dengan peringatan
"not implemented" sekali); tekstur 1D praktis tidak punya mip. Kontrol: x/y
tertukar pada 2D.LL menggagalkan tes. Fetch di luar batas tidak dibandingkan.

## Tahap C: gather TLD4S (baru)

`tests/gather_reference.py` (2400 gather, 300 konfigurasi): TLD4S melalui sampler TSC
acak (mode alamat wrap/mirror/clamp) mengembalikan komponen yang dipilih dari
footprint 2x2 dengan urutan standar (x0,y1),(x1,y1),(x1,y0),(x0,y0); semua cocok,
tanpa bug. Kontrol: urutan footprint dari kiri-atas menggagalkan tes. Belum:
offset (AOFFI/PTP), depth compare, border, TXQ, TMML, TXD.

## Tahap C: IADD3, LEA.LO, LEA.HI (baru)

Oracle ALU menjadi 15.838 kasus, semua cocok: IADD3 (pilihan setengah-kata, negasi
per operand, shift kanan/kiri 16 pada jumlah a+b; semantik shift mengikuti
konvensi komunitas, bukan teks NVIDIA), LEA.LO (shift 0-31, negasi) dan LEA.HI
(kata tinggi dari nilai 64-bit yang digeser, termasuk negasi 64-bit). Tidak ada
bug. Kontrol: shift tambahan menggagalkan tes. X/CC/predikat keluaran tidak diuji
(Hydra belum mengimplementasikannya). ICMP/FCMP/I2I/PSET tidak diuji karena
semantiknya tidak terdefinisi di sumber yang ada.

## Tahap D: konversi tulis render target (baru)

`tests/color_target_format_reference.py` kini mencakup 33 format RT lagi yang
keluarannya float (selain 0x40-0x47 yang sudah ada): RGBA32F/RGBX32F, RGBA16
unorm/snorm/float, RG32F, RGBX16F, A8R8G8B8 (+sRGB), A2B10G10R10, A2R10G10B10,
A8B8G8R8 sRGB/snorm, RG16 unorm/snorm/float, BF10GF11RF11, R32F, X8R8G8B8 (+sRGB),
R5G6B5, A1R5G5B5, G8R8 unorm/snorm, R16 unorm/snorm, R8 unorm/snorm, X1R5G5B5,
X8B8G8R8 (+sRGB). Tata letak diturunkan dari nama format di `clb197.h` (garis bawah
= urutan memori, nama rapat = bit tertinggi dahulu), 24 warna per format, toleransi
satu langkah kuantisasi (float32 eksak, bit X diabaikan). Semua cocok, tanpa bug;
kontrol: kanal R/B tertukar pada A2B10G10R10 dan R5G6B5 menggagalkan tes. Format
integer, Z/O, dan blending ke format-format ini tidak diuji.

## Tahap C: rantai carry integer (LEA/ISCADD .X/.CC) dan peta cakupan instruksi

**Audit (dari decoder, oracle Python, dan tes C++).** Status per keluarga ALU GPU:

| Keluarga | Status | Bukti / batas |
|---|---|---|
| FADD/FMUL/FFMA/FMNMX, MUFU, FSETP/ISETP, ISET/FSET, SEL, BFE/BFI, XMAD, IADD, IADD3 (tanpa X/CC), LEA.LO/HI, LOP/LOP3, SHL/SHR/IMNMX/POPC/FLO, HADD2/HMUL2/HFMA2 | VERIFIED (model Python) | `maxwell_alu_reference.py`, `maxwell_predicate_reference.py`; denormal fp32 = GAP host yang tercatat |
| SHF (funnel shift) | VERIFIED (tes C++ tulisan tangan) | `shader_library_tests.cpp`; belum ada model Python |
| **LEA.LO/LEA.HI `.X`/`.CC`, ISCADD `.CC`** | **VERIFIED untuk carry C (baru)** | bagian ini; sebelumnya dihitung tanpa carry dengan hanya log |
| IADD `.X`/`.CC`, XMAD `.X`/`.CC` | IMPLEMENTED_UNVERIFIED | kode ada; belum dibandingkan dengan model |
| IADD3 `.X`/`.CC` | MISSING (kini dilaporkan) | sebelumnya diabaikan diam-diam; carry tiga operand bisa 2 sehingga satu flag C tidak cukup; semantik perlu bukti hardware |
| LEA/LEA.HI keluaran predikat (`dst_pred != PT`), ISCADD.PO + CC, SHL/SHR/SHF X/CC | MISSING (dilaporkan `LOG_NOT_IMPLEMENTED`) | |
| ICMP/FCMP, I2I, PSETP/PSET, R2P, FSWZADD, VOTE | IMPLEMENTED_UNVERIFIED / sebagian lewat tes shader-library | tanpa model independen |

**Celah yang dipilih.** Penjumlahan 64-bit pada shader (alamat LDG/STG, offset buffer) dikompilasi sebagai
`LEA.CC` lalu `LEA.HI.X`. Hydra memperlakukan `.X` dan `.CC` sebagai tidak ada: kata tinggi salah
tanpa kegagalan (hanya satu baris log). `IADD3.X/.CC` bahkan tanpa log.

**Perubahan** (`decoder/integer_arithmetic.cpp`): helper `addWithCarry` (carry masuk dari C, flag Z/S/C/O
seperti IADD.CC) dipakai oleh LEA.LO, LEA.HI, ISCADD dan ISCADD32I. Pembacaan carry masuk terjadi sebelum
flag ditulis, jadi `.X.CC` memakai C lama. IADD3 `.X/.CC` dan keluaran predikat LEA kini
`LOG_NOT_IMPLEMENTED`, tidak lagi diam.

**Tes** (`maxwell_alu_reference.py`, `make_carry_cases`, 968 kasus baru; total 16.806, semua cocok): model
integer besar Python untuk
(a<<s)+b 64-bit (shift 0-31, negasi, carry masuk acak), carry masuk X-saja untuk LEA.LO/HI, carry masuk
diabaikan tanpa `.X` dengan semua flag tak berubah, ISCADD.CC, dan jumlah 96-bit tiga limb dengan `.X.CC`.
Setiap register masukan yang bukan keluaran harus tak berubah. Kontrol: dengan decoder lama 5 dari 6 grup
gagal (carry_lea64 111/320, carry_96bit 48/300).

**Batas.** Hanya flag C dibandingkan; Z/S/O pada LEA/ISCADD mengikuti konvensi IADD.CC dan tidak ada
sumber primer. Semantik `.X`/`.CC` pada SASS tidak dipublikasikan NVIDIA; acuan adalah identitas aritmetika
64-bit yang harus dipenuhi pasangan instruksi, bukan trace hardware. Regresi: shader-library 59, gpu-engine
45 (MTL_DEBUG_LAYER=1), predicate oracle 499.296 kasus. Belum diuji di game; tidak di-stage/commit.
Worktree saat sesi ini bersih (perubahan sebelumnya sudah ter-commit), hanya `tests/__pycache__/` yang untracked.

## Fault terlacak dan peta cakupan (lanjutan)

Fitur yang dilewati decoder/analyzer/emitter kini tercatat per shader (`SHADER_UNSUPPORTED`, 
`ResourceMapping::unsupported`, satu baris WARN di `ShaderCache`), termasuk opcode yang belum didekode
(`Opcode <nama>`). Kebijakan: catat lalu lanjut. Peta lengkap per keluarga, status, bukti dan fault id:
[instruction-coverage-2026-10-20.md](instruction-coverage-2026-10-20.md). Oracle ALU menjadi 17.206 kasus
(`iadd_cc`, `iadd_cc_x`, `iadd32i_cc*`, `iadd_chain64`: flag Z/S/C/O dan rantai 64-bit), semua cocok.
IADD dengan operand dinegasi diperbaiki (`~b` + carry-in, bukan `-b`; dasarnya identitas pengurangan 64-bit, bukan hardware); oracle 17.738 kasus.
Regresi: `run_python_references.sh` ALL PASS, shader-library 60, gpu-engine 45, texture-cache 40
(MTL_DEBUG_LAYER=1). Header berubah (`ResourceMapping`, `shader_decompiler/const.hpp`): rebuild `hydra` +
hot module dan restart aplikasi. Tidak stage/commit.

## Sinkronisasi: race completion/cancel dan acquire 3D (diperiksa dari audit eksternal)

Dua temuan audit eksternal diperiksa langsung di kode; keduanya nyata.

1. **Completion vs cancel** (`pfifo.cpp`, callback `onCompletedChecked`): `status()==Running` dicek, lalu
   `complete()` (menaikkan fence guest) dipanggil, lalu `finish(true)`; `cancel()` dari thread lain bisa menyela,
   sehingga fence terbit untuk pekerjaan yang berstatus Cancelled. Kini `WorkTicket::claimCompletion()` mengambil hak
   publikasi secara atomik (hanya dari Running, sekali); `cancel()` setelah klaim tidak berlaku, dan klaim gagal bila
   sudah dibatalkan. Tes: `work_manager_tests` (klaim, urutan, dan 4.000 putaran dua thread; publikasi hanya bila
   status akhir Succeeded).
2. **Acquire 3D memblokir** (`ThreeD::setReportSemaphore`, operasi 1): polling hingga 1 detik di worker PFIFO, lalu
   lanjut diam-diam. Dengan decode state kini channel menitipkan `semaphore_wait` (operasi 1 = sama, 4 = >=, seperti
   acquire puller) dan melanjutkan setelah terpenuhi; polling berbatas hanya untuk pemanggil tanpa scheduler (tes).
   Konsekuensi: acquire yang tidak pernah dirilis sekarang menahan channel itu (seperti puller dan hardware), tidak
   lagi lolos setelah 1 detik. Tes: `testReportSemaphoreReductionAndAcquire`.

Regresi (MTL_DEBUG_LAYER=1): shader-library 60, texture-cache 40, gpu-engine 45, work-manager, resource-registry,
raster-state, render-hash, register-write, frame-trace, visual-diagnostic, dma-slice lulus; `hydra` dan hot module
ter-build. Header `work_manager.hpp` berubah: restart aplikasi. Belum diuji di game; tidak stage/commit.
