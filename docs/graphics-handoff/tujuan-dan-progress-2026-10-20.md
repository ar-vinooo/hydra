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
