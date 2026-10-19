# Audit penutupan tahap A — 19 Oktober 2026

**Status: OPEN.** Permintaan pengguna adalah menuntaskan seluruh tahap A dalam
satu rangkaian kerja. Audit ini mencatat hasil implementasi dan gate yang
masih menghalangi penutupan; hasil regresi bukan pengganti bukti semantik.

## Pekerjaan paket audit ini

| Area | Perubahan | Batas bukti |
|---|---|---|
| nvmap metadata | Reader memegang snapshot immutable yang tetap valid sesudah FREE/ALLOC; lookup tidak mengalokasikan snapshot baru | Snapshot metadata tidak mem-pin setiap jenis backing fisik lintas proses |
| nvmap per-klien | CREATE/FROM_ID/FREE dan FD closure melacak refs; import invalid/unallocated dan akses tanpa import ditolak | Import lintas proses tetap NotSupported |
| Identitas owner | Token MMU ditahan untuk membedakan proses baru yang kebetulan memakai alamat object proses lama | API handle-table kernel secara umum belum bebas race lookup/close |
| nvmap ABI | Seluruh ukuran input/output dicek; IPC unaligned disalin ke scratch aligned; output gagal tidak dipublikasikan | Kode error dan field FREE khusus Switch belum disertifikasi hardware |
| nvmap metadata alloc | Alignment minimum 4 KiB, alignment/kind aktual disimpan; tidak memakai konstanta big-page 128 KiB sebagai minimum | Eksekusi compression kind tidak diimplementasikan |
| DeviceShared | QueryMemory menghitung mapping hidup, alias dan lintas AS; sparse/padding tidak dihitung sebagai guest backing | Jumlah refs mengikuti model host, bukan hasil trace kernel Horizon |
| Lifetime DeviceShared | Registry state terpisah dari MMU; AS dapat ditutup sesudah proses tanpa callback ke MMU yang sudah mati | Seluruh API alias CPU dan perubahan mapping CPU serentak belum disertifikasi |
| GET_VA_REGIONS | Inline ioctl1 dan buffer kedua ioctl3 diisi dari pool allocator; ukuran/pointer IPC diperiksa | Default unified VA/40-bit adalah kebijakan Hydra; bukan nilai default Switch yang sudah dibuktikan |
| AS allocation | Pool split eksplisit dipakai allocator small/big; ukuran input/output seluruh ioctl AS yang ditangani diperiksa | Seluruh flag InitializeEx dan exact Switch error masih terbuka |
| Mapping replacement | Penggantian mencabut seluruh range lama, termasuk tail yang menjadi padding; Remap menyimpan hasil validasi sebelum apply | Modifikasi mapping CPU serentak dan OOM transactional rollback seluruh batch belum selesai |
| Submit bit 5 | SKIP_BUFFER_REFCOUNTING diterima; tidak mengubah eksekusi/fence | Hydra tidak melakukan scan refs per-submit; backing tetap ditahan secara konservatif |
| Modify/kind | Permintaan yang belum dimodelkan mengembalikan NotSupported | Penolakan bukan implementasi perubahan PTE kind |

## Gate yang masih wajib ditangani

1. **FROM_ID lintas proses dan kepemilikan backing fisik (keputusan: tetap
   NotSupported).** Hydra hanya membuat satu proses guest yang memakai nvdrv;
   tidak ada konsumen yang ditemukan, jadi representasi backing bersama tidak
   dibangun tanpa bukti kebutuhan. Rincian risiko di bawah tetap berlaku. Metadata sekarang
   aman, tetapi alamat guest proses A tidak boleh dibaca melalui MMU proses B.
   Membuka kasus ini memerlukan representasi backing bersama yang tetap sah
   setelah proses asal berakhir, termasuk heap, shared memory, dan alias memory.
   Jangan menghapus pemeriksaan owner lalu menganggap import selesai.
2. **Modify/compression kind (diimplementasikan sebatas model Hydra; diverifikasi
   di Crash 4).** NVN memanggil MapBufferEx+Modify dengan handle nvmap null untuk
   mengubah kind mapping yang sudah ada (contoh log: offset 0, size 0x10000,
   address 0x100880000). Hydra menyimpan memori tanpa kompresi dan tidak memakai
   kind untuk tata letak, jadi Modify hanya memvalidasi bahwa alamat+ukuran jatuh
   di dalam mapping yang ada (selain itu BadParameter, dengan log) dan tidak
   mengubah mapping. Versi pertama yang me-resolve handle membuat Crash 4 abort
   di `Texture->Initialize`; setelah diperbaiki pengguna mengonfirmasi game
   masuk. Kompresi sungguhan tetap tidak ada.
3. **Mutasi memory ketika masih live (ditutup 19 Oktober malam).** Heap kini
   satu reservasi host 4 GiB yang di-resize di tempat: tidak ada salinan,
   pointer host tetap, GPU mapping tidak perlu di-rebase saat tumbuh. Tes
   `--check-gpu-heap-resize` menjalankan thread penulis selama 64 resize dan
   tidak ada tulisan yang hilang (belum ada kontrol negatif terhadap kode lama).
   Race handle-table lookup/close umum tetap di luar cakupan.
4. **Kontrak Switch yang belum terkonfirmasi.** Field FREE, exact error dan
   precedence, default VA pools, seluruh flags AS, serta reset/recovery pada
   channel yang sama memerlukan kontrak/traces yang lebih kuat. Jangan
   mengimpor nomor ioctl Linux FORCE_RESET ke Switch tanpa bukti ABI.
5. **Encoding di luar kontrak terpublikasi.** END_SUBDEVICE_MASK tidak ada
   dalam header Maxwell atau daftar PB control entry manual Turing yang
   dibaca. Hydra tetap mem-fault encoding yang tidak didukung. Tidak ada bukti
   yang cukup untuk menyebutnya fitur Maxwell yang sudah diimplementasikan
   maupun untuk membuktikan semua encoding undocumented tidak berlaku.
6. **Validasi gameplay/hardware/performa.** Python adalah model independen;
   Metal mengeksekusi GPU host asli. Belum ada eksekusi NVIDIA fisik untuk
   gate ini. Pengguna menjalankan gameplay; sesi ini tidak mengoperasikan GUI.

## Sumber yang dipakai

- [Header Maxwell channel](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/host/clb06f.h),
  [manual PBDMA Turing](https://github.com/NVIDIA/open-gpu-doc/blob/master/manuals/turing/tu104/dev_pbdma.ref.txt),
  dan [manual FIFO DMA RAM](https://github.com/NVIDIA/open-gpu-doc/blob/master/manuals/turing/tu104/dev_ram.ref.txt).
  Manual keluarga berikutnya bukan trace GM20B.
- Kode berhak cipta NVIDIA pada
  [nvmap_handle.c](https://android.googlesource.com/kernel/tegra/+/5028b8088fd5743c23f4566678e2eac867fa50f3/drivers/video/tegra/nvmap/nvmap_handle.c)
  dan [nvmap_ioctl.c](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/drivers/video/tegra/nvmap/nvmap_ioctl.c)
  untuk validasi handle dan duplikasi/refcount klien. Versi driver memakai
  beberapa bentuk ID/FD; tidak membuktikan nilai handle Switch harus berbeda
  antar-klien. Hydra mempertahankan ID global dengan refs per-klien.
- Kode NVIDIA r32 pada mirror OE4T:
  [channel.c](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/nvgpu/drivers/gpu/nvgpu/common/fifo/channel.c),
  [submit.c](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/nvgpu/drivers/gpu/nvgpu/common/fifo/submit.c),
  [ioctl_as.c](https://github.com/OE4T/linux-tegra-4.9/blob/oe4t-patches-l4t-r32.7.4/nvidia/nvgpu/drivers/gpu/nvgpu/os/linux/ioctl_as.c).
  Bit 5 melewati pengambilan refs buffer per job. VA region melaporkan base,
  page size dan panjang allocator dalam pages.
- [libnx nvmap caller](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/ioctl/nvmap.c)
  dan [AS caller](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/ioctl/nvhost-as-gpu.c)
  menetapkan layout/transport caller. libnx menamai field FREE `refcount`,
  sedangkan [Switchbrew](https://switchbrew.org/wiki/NV_services#NVMAP_IOC_FREE)
  mendeskripsikan alamat/flag uncached. Field tersebut diabaikan wrapper
  libnx, sehingga labelnya tidak membuktikan semantik output kernel.

Hash sumber sesi disimpan dalam
`/tmp/hydra-stage-a-final/source-manifest.json`.

## Mengulang regresi

```sh
ninja -C build/swiftui-release -j 6 hydra hydra-hot hydra-shutdown-tests \
  hydra-gpu-engine-tests hydra-xci-tests hydra-texture-cache-tests hydra-raster-state-tests
python3 tests/stage_a_regression.py \
  --nvidia-header /tmp/hydra-nvidia-clb06f.h \
  --nvidia-manual /tmp/hydra-nvidia-dev_pbdma.ref.txt \
  --output /tmp/hydra-stage-a-final
```

Runner mengaktifkan Metal validation, mematikan hot reload, menjalankan
selector instruksi, suite terkait, empat model Python dan kontrol negatif.
Lihat `summary.json` untuk hasil aktual; status penutupan tetap mengikuti gate
di atas. Tidak stage/commit.
