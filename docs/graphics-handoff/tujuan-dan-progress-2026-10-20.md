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
