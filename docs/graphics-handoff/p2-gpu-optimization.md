# Implementasi P2 GPU

P2 sekarang memiliki implementasi source, terpisah dari status pengaktifannya.
Build dan pengujian belum dijalankan pada kelanjutan Windows ini. Tidak ada
hasil performa, kelulusan P0 atau kesetaraan perangkat yang diklaim.

## Perubahan

| Rekomendasi laporan | Implementasi |
| --- | --- |
| Combine command buffers / batch draws | `Pfifo::threadFunc` memakai satu wrapper untuk daftar berurutan dari channel dan address space yang sama, maksimum 8 daftar / 65.536 word |
| Pertahankan dependency graph | Tidak ada pengurutan ulang; batas encoder antardaftar tetap dipertahankan; flush saat wait, fence boundary, fault, cancel, pergantian context, budget penuh atau antrean habis |
| Lifetime / completion batching | Semua resource dan callback tetap ditahan; retirement decode dipublikasikan setelah native commit; cancel menunggu batch yang belum di-commit sebelum memory boleh dibebaskan |
| Coalesce dirty ranges | `resource::coalesceRanges` membentuk union tepat dari rentang overlap/adjacent; celah yang tidak disentuh tidak ikut diinvalkan |
| Kurangi pekerjaan invalidasi | Versi dan event CPU ditulis satu per satu sesuai urutan semula; hanya pekerjaan eviction cache pada batas invalidasi yang digabung |
| Kurangi checkpoint berulang | Sinkronisasi storage memakai epoch issued/synchronized milik backend untuk melewati wait yang sudah terpenuhi; pending write tetap disinkronkan |
| Gabungkan bookkeeping GPU writes | Rentang write epoch yang sama digabung sebelum stamping ownership; event origin dan completion tetap dipertahankan |
| Reuse views / defer uploads / avoid unnecessary readback | Tetap memakai `TextureCache::getTextureView` yang membandingkan descriptor, upload saat lookup resource diperlukan, snapshot buffer immutable dan pemeriksaan byte di `BufferCache::updateRangeHot2`; perubahan P2 tidak menonaktifkan mekanisme alias/writeback yang diperlukan |

Batching menyatukan submission native. Ini tidak mengubah beberapa draw menjadi
satu draw, tidak memperkenalkan multi-queue execution, dan tidak menghapus
encoder boundary dengan asumsi bahwa descriptor yang sama berarti tidak ada
hazard. Checkpoint guest semaphore dan pembacaan CPU tetap berlaku.

## Prasyarat aktivasi sesuai laporan

Laporan meminta optimasi setelah test P0 hijau. Source P2 tetap tersedia tanpa
memalsukan kelulusan tersebut. Runtime memilih jalur konservatif sampai mode
optimasi diminta dan receipt valid tersedia.

Pada macOS, konfigurasi dengan `HYDRA_HOT_RELOAD=OFF`, kemudian jalankan ketika
pengujian sudah diizinkan:

```sh
python3 tools/validate_gpu_correctness.py --require-metal --enable-optimizations \
  --build-dir build/swiftui-release
```

Script menjalankan pemeriksaan portable dan baseline native, memeriksa identitas
source build, lalu menjalankan lagi GPU-engine, compute profiling, PFIFO,
channel scheduling, process cancellation, batching dan 1.000 iterasi stress pada
mode optimasi. Receipt sementara dihapus setelah proses; receipt aktivasi akhir
hanya diterbitkan bila seluruh tahap yang diwajibkan lulus dan source tidak
berubah selama pemeriksaan. Kegagalan menghapus receipt hasil run tersebut.

Setelah kelulusan nyata, jalankan proses baru dengan:

```sh
export HYDRA_GPU_OPTIMIZATIONS=1
export HYDRA_GPU_OPTIMIZATION_RECEIPT="$PWD/build/correctness/gpu-optimization.receipt"
```

`HYDRA_GPU_OPTIMIZATIONS=0` atau tidak menyetel variabel mempertahankan jalur
konservatif. Receipt hilang, format salah, fingerprint berbeda atau build hot
reload juga mempertahankan jalur konservatif. Policy dipilih sekali per proses;
restart diperlukan setelah mengubah konfigurasinya.

CMake dan `tools/gpu_optimization_gate.py` menghitung SHA-256 dari daftar source
GPU/CPU-memory/hot, skenario test, tools dan file konfigurasi yang dicakup.
Normalisasi CRLF memungkinkan identitas teks yang sama di Windows/macOS.
Perubahan source yang dicakup memicu konfigurasi ulang dan membuat receipt lama
tidak cocok dengan binary baru. Ini mekanisme pengikatan bukti uji ke build,
bukan autentikasi terhadap pemilik mesin yang sengaja memalsukan receipt.

## Source pemeriksaan yang ditambahkan

`tests/range_set_tests.cpp` memeriksa union rentang, celah, versi ownership,
completion terlambat setelah overwrite CPU, serta edge RAW/WAR/WAW terhadap
referensi tanpa coalescing. `tests/optimization_policy_reference.py` menjalankan
policy produksi di proses baru untuk receipt hilang, salah, terpotong, tambahan
record, disabled dan valid.

Selector native `--check-command-buffer-batching` menahan 16 daftar pada gate
yang sama, lalu memeriksa seluruh completion, sequence dan register akhir. Test
mengharapkan 16 native submission pada baseline dan 3 pada mode batching
terbatas. Pengujian tersebut merupakan source yang belum dijalankan di sesi ini.
