# Referensi pengembangan Hydra

Daftar dari pengguna, disimpan pada 18 Oktober 2026. Gunakan bersama aturan
bukti di [README](README.md). Tautan tersedia bukan berarti seluruh isinya
sudah diaudit atau setiap perilaku Hydra sudah sesuai dengannya.

## CPU

- [ARM Architecture Reference Manual — DDI0487](https://support.arm.com/documentation/ddi0487)
- [ARM Learn the Architecture — A-profile](https://www.arm.com/architecture/learn-the-architecture/a-profile)

ARM adalah sumber primer arsitektur CPU. Pilih revisi dan fitur yang sesuai
dengan CPU guest; jangan menerapkan fitur ARM baru ke Tegra X1 secara otomatis.

## GPU

- [Switchbrew GPU](https://switchbrew.org/wiki/GPU)
- [Switchbrew GPU Classes](https://switchbrew.org/wiki/GPU_Classes)
- [envytools Maxwell CUDA ISA](https://envytools.readthedocs.io/en/latest/hw/graph/maxwell/cuda/isa.html)
- [envydis / envyas](https://envytools.readthedocs.io/en/latest/envydis/index.html)
- [NVIDIA open-gpu-doc](https://github.com/NVIDIA/open-gpu-doc)

Switchbrew dan envytools membantu penelusuran hasil reverse engineering.
Untuk menyatakan semantik hardware terverifikasi, cocokkan dengan
dokumentasi/header NVIDIA atau hardware trace yang dapat direproduksi.
Referensi spesifik PFIFO/notifier ada di
[README PFIFO](../../tests/README-pfifo.md).

## Horizon OS

- [Switchbrew SVC](https://switchbrew.org/wiki/SVC)
- [Switchbrew Services API](https://switchbrew.org/wiki/Services_API)
- [switchbrew/libnx](https://github.com/switchbrew/libnx)

Gunakan untuk menelusuri ABI, layout request/response, command ID, dan urutan
pemanggilan. Source libnx membuktikan perilaku caller libnx tersebut; perilaku
SDK game dan layanan guest tetap perlu dicocokkan dengan log atau pengujian.

## Audio

- [Switchbrew Audio services](https://switchbrew.org/wiki/Audio_services)

## Host macOS / Metal

- [Apple Metal documentation](https://developer.apple.com/documentation/metal)
- Header SDK lokal dan hasil Metal API Validation.

Untuk setiap perbaikan, catat halaman/simbol/revisi yang mendasari perilakunya,
tes yang dijalankan, dan bagian yang masih berupa hipotesis. Daftar ini tidak
mengubah posisi pekerjaan: tahap instruksi masih OPEN dan validasi startup
gambar setelah perbaikan reservasi increment/level masih menunggu hasil pengguna.

## Catatan penggunaan (audit 18 Oktober 2026)

Switchbrew NV services dan SVC sudah diperiksa untuk AS_GPU, flag submit,
syncpoint, dan SetHeapSize; ringkasan cocok/asumsi/belum ada di
[README PFIFO](../../tests/README-pfifo.md), bagian "Audit sumber". Halaman
tersebut jarang mendokumentasikan error code atau reuse, jadi perilaku itu
harus tetap diberi label kebijakan Hydra, bukan perilaku Switch terverifikasi.
Referensi yang belum dipakai: Switchbrew GPU/GPU Classes, envytools, Audio, ARM.
