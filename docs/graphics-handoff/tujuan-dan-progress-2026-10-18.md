# Tujuan, alur render, dan posisi pekerjaan Hydra

Checkpoint percakapan: **18 Oktober 2026**. Dokumen ini menjadi titik masuk
untuk melanjutkan pekerjaan lintas sesi. Detail teknis jalur instruksi ada di
[tests/README-pfifo.md](../../tests/README-pfifo.md); riwayat pekerjaan grafis
yang lebih luas ada di [README.md](README.md) dan [backlog.md](backlog.md).

## Tujuan yang disepakati

Memperbaiki jalur grafis Hydra dari instruksi game sampai gambar ditampilkan,
dengan memahami dan memeriksa setiap tahap secara berurutan. Hydra menerjemahkan
perintah Tegra X1/Maxwell GM20B milik Nintendo Switch ke Metal di macOS.

Prioritas: kebenaran perilaku, kompatibilitas luas, kemudahan perawatan, lalu
performa. Perbaikan harus mengatasi penyebab di tingkat engine, bukan bergantung
pada nama game atau menambal satu gejala visual.

**Kesepakatan terpenting dari pengguna:** jangan pindah ke tahap berikutnya
ketika tahap sebelumnya masih memiliki bug atau implementasi wajib yang belum
ditangani. Tujuannya mengurangi pekerjaan bolak-balik mencari penyebab ketika
sudah berada di ujung jalur render.

Build berhasil, tes terarah lulus, atau satu game terlihat benar belum cukup
untuk menyebut seluruh tahap selesai. Catat cakupan yang terbukti dan batasnya.
Perilaku yang belum didukung harus dilaporkan secara eksplisit; penolakan aman
belum berarti fitur tersebut telah diimplementasikan.

## Peta dari awal sampai hasil

```text
Game berjalan pada CPU guest
  -> game/driver menyusun command buffer dan descriptor di memori guest
  -> layanan nvdrv menerima submit GPFIFO
  -> validasi header, pointer, panjang, flags, dan fence
  -> antrean PFIFO memilih channel yang siap
  -> PFIFO membaca packet menjadi method + argument
  -> puller menangani binding, semaphore, syncpoint, dan urutan pekerjaan
  -> method dikirim ke engine 3D / Compute / Copy / 2D / Inline
  -> engine membaca register, shader, texture, buffer, dan render target
  -> shader Maxwell diterjemahkan menjadi IR lalu MSL
  -> renderer membentuk pipeline dan encode command buffer Metal
  -> GPU host mengeksekusi draw / dispatch / copy
  -> hasil tersimpan pada render target; sinkronisasi menjaga visibilitasnya
  -> buffer queue / compositor / present
  -> gambar terlihat pada layar
```

Alur ini tidak selalu berupa satu garis: compute dan copy dapat menghasilkan
data untuk draw berikutnya; semaphore/fence menghubungkan dependensi; cache
harus menjaga isi buffer dan texture tetap konsisten.

Shadow dan lighting umumnya merupakan hasil shader, texture/depth, render
target, serta beberapa pass yang diperintahkan game. Keduanya bergantung pada
ketepatan jalur di atas dan belum dinyatakan selesai hanya karena 3D atau
texture sudah mendapat perbaikan.

| Tahap | Cakupan | Posisi checkpoint ini |
|---|---|---|
| A. Jalur instruksi | Submit, decode, state channel, dependency, fault, lifetime memori | **Sedang dikerjakan; OPEN** |
| B. Engine dan resource | Register, draw/dispatch, descriptor, buffer, texture | Ada perbaikan terarah; audit menyeluruh belum selesai |
| C. Shader dan pipeline | Maxwell → IR → MSL, state raster/depth/blend | Ada pekerjaan terdahulu; kelengkapan belum dinyatakan |
| D. Eksekusi dan hasil pass | Metal, coherence, render target, shadow/lighting | Sebagian perilaku diuji; masih ada batasan |
| E. Presentasi | Buffer queue, compositor, present | Tahap lanjutan; belum ditutup oleh pekerjaan instruksi |

## Urutan pembahasan dan pekerjaan sesi ini

1. Pengguna meminta pemahaman alur instruksi GPU sampai hasil render dan
   pemeriksaan bug program, termasuk texture, shadow, dan lighting.
2. Pemeriksaan kode menemukan pembacaan QMD compute yang belum aman serta
   celah konsistensi storage buffer dengan texture. Perbaikan terarah dilakukan.
3. Pengguna meminta peta lengkap agar jelas awal, akhir, dan posisi pekerjaan.
4. Fokus diarahkan kembali ke awal jalur: submit GPFIFO, decoder PFIFO,
   semaphore, fence, dan urutan eksekusi.
5. Pengguna menegaskan bahwa tahap lama harus dituntaskan sebelum melanjutkan.
   Audit diperluas ke antar-channel, fault, pembatalan proses, dan pointer guest.
6. Build dan tes di bawah lulus. Status jalur instruksi tetap **OPEN**, karena
   masih ada fitur dan jaminan lifetime yang belum lengkap.
7. Pengguna meminta dokumentasi agar pekerjaan bisa dilanjutkan kapan saja.

Banyaknya `if/else` sendiri bukan bukti tambalan: decoder memang perlu
membedakan opcode, format, dan state. Penilaiannya harus berdasarkan semantik,
konsistensi state, penanganan error, dan bukti tes. Belum ada kesimpulan bahwa
seluruh jalur instruksi kini sudah rapi atau bebas bug.

## Perbaikan yang sudah dilakukan dan diuji

| Area | Perubahan dan cakupan |
|---|---|
| Compute QMD | Descriptor dibaca melalui mapping yang diperiksa; mapping hilang, terpotong, atau terpisah tidak dibaca dengan memcpy pointer mentah |
| Buffer ↔ texture | Storage binding mem-flush texture dirty sebelumnya; setelah storage write, checkpoint GPU dan invalidasi texture menjaga hasil berikutnya tidak memakai isi lama; partial overwrite direkonsiliasi juga saat reuse render target |
| Input GPFIFO | Validasi header/output/seluruh entry array, overflow, input tidak aligned, dan submit kosong; ioctl1 membaca entry setelah header IPC dan mengabaikan pointer, ioctl2 membaca entry inline |
| Decode PFIFO | Membatasi baca sesuai panjang entry; checked read lintas mapping; packet dan argumen macro yang belum lengkap disimpan lintas entry/submit |
| Isolasi channel | Setiap channel memiliki decoder, engine, binding subchannel, macro state, puller register, dan completion fence sendiri |
| Scheduling | Wait dapat yield agar channel lain berjalan; cursor disimpan; pekerjaan berikutnya pada channel yang sama tidak menyalip; retirement mengikuti prefix berurutan |
| Semaphore/syncpoint | WFI menunggu pekerjaan Metal sebelumnya; ukuran release/timestamp ditangani; acquire tidak dianggap selesai karena timeout; wait/increment syncpoint A/B diuji |
| Fault | Mapping invalid dan command unsupported mem-fault channel; event dan polling notification tersedia; pekerjaan gagal tidak menerbitkan completion sukses |
| Lifetime proses | Cleanup membatalkan antrean, menunggu decoder berhenti memakai GMMU, lalu drain pekerjaan Metal sebelum alokasi guest dibebaskan |

Sinkronisasi storage saat ini menggunakan checkpoint sinkron. Ini merupakan
fallback kebenaran dan dapat menambah stall; optimasi GPU-only belum dilakukan.
Rincian storage dan batas shader ada di
[global-memory-2026-10-17.md](global-memory-2026-10-17.md).

Lokasi kode utama:

- `src/core/horizon/services/nvdrv/ioctl/nvhost_gpu.{cpp,hpp}` dan `channel_base.hpp`
- `src/core/hw/tegra_x1/gpu/pfifo.{cpp,hpp}`, `gpu.{cpp,hpp}`, `gmmu.hpp`
- `src/core/hw/tegra_x1/gpu/engines/3d.{cpp,hpp}` dan `compute.{cpp,hpp}`
- `src/core/hw/tegra_x1/gpu/renderer/texture_cache.{cpp,hpp}`
- `src/core/hot/texture_cache_hot.cpp`
- `src/core/horizon/kernel/process.cpp`
- `tests/shutdown_tests.cpp` dan `tests/gpu_engine_tests.cpp`

## Bukti build dan pengujian terakhir

Pada sesi 18 Oktober 2026, build aplikasi dan target tes berikut berhasil:

```sh
ninja -C build/swiftui-release -j 6 hydra-shutdown-tests hydra-gpu-engine-tests hydra \
  hydra-texture-cache-tests hydra-raster-state-tests
```

Enam pemeriksaan integrasi lulus dengan `MTL_DEBUG_LAYER=1`, melalui executable
`build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests`:

- `--check-gpfifo-input`
- `--check-pfifo-decode`
- `--check-channel-scheduling`
- `--check-channel-faults`
- `--check-gpu-process-cancel`
- `--check-semaphore-shutdown`

Tiga suite regresi `hydra-gpu-engine-tests`, `hydra-texture-cache-tests`, dan
`hydra-raster-state-tests` juga lulus dengan Metal validation. Full shutdown
suite tanpa argumen dan `git diff --check` lulus. Perintah lengkap serta
penjelasan cakupannya tersedia di [README PFIFO](../../tests/README-pfifo.md).

Aplikasi build: `build/swiftui-release/bin/Hydra.app`.
**Game dan GUI tidak dijalankan pada sesi ini.** Hasil tes bukan bukti
kompatibilitas gameplay atau kelengkapan seluruh fitur Maxwell/NVN.

## Pekerjaan terbuka sebelum tahap A boleh ditutup

**Laporan gameplay setelah checkpoint:** pengguna mencoba game dan semuanya
masih 0 FPS. Log percobaan title `0100f9f00c696000` menunjukkan
`SET_ERROR_NOTIFIER` mengembalikan NotSupported, diikuti SDK data abort saat
inisialisasi grafis. Ini regresi kompatibilitas dari perubahan sebelumnya.
Registrasi notifier sekarang diimplementasikan pada Apple Hypervisor dengan
shared VM alias yang memiliki backing memori untuk callback tertunda; tes
registrasi, publikasi error, dan lifetime lulus. Lima tes instruksi lainnya
serta full shutdown juga lulus. Build aplikasi berhasil. Percobaan pengguna
berikutnya mencapai menu bersuara, input bekerja, dan angka 60 FPS, tetapi
viewport kosong. Log serta inspeksi argumen proses menunjukkan submit ioctl2
dengan flag `HwFormat` (`0x4`) ditolak whitelist. Format native ini sekarang
diterima melalui decoder yang ada; tes ioctl1/ioctl2 memeriksa efek register
serta completion dengan/tanpa FenceGet. Build dan empat tes instruksi terkait
lulus. Percobaan berikutnya tetap gelap; log memperlihatkan flag awal `0x104`
ditolak lalu error entry conditional/subroutine dan channel InvalidState.
Bit 8 sekarang mereservasi jumlah increment guest tanpa melakukan completion
palsu. Penolakan LEVEL_SUBROUTINE juga diperbaiki: level ini mengatur pelaporan
progres, bukan call/return. Log lama belum membedakan bit penyebab fault;
conditional fetch tetap belum didukung dan log barunya mencatat raw entry.
Pengguna kemudian melihat frame bolak-balik/stuck, sementara audio/input sudah
masuk menu. Log menunjukkan counter syncpoint tetap 0 saat menunggu threshold
153–158, kemudian binding engine invalid dan error 8 (InvalidState). Ditemukan
handler 3D INCREMENT_SYNC_POINT (0xb2) belum diimplementasikan; kini counter
naik setelah checkpoint GPU berhasil. Tes GPU write sungguhan, integrasi fence
0x104, indeks invalid, dan regresi terkait lulus; build berhasil. Hubungan
kausal dengan binding invalid/frame bolak-balik masih perlu dibuktikan lewat
percobaan pengguna. **Gameplay setelah perbaikan syncpoint 3D belum diverifikasi.**
Percobaan berikutnya membuktikan counter sudah naik, tetapi layar autosave CTR
masih berulang sementara audio/input di menu. Fault binding 0x40 tetap muncul;
header sumbernya 0x2d953000 pada GPU 0x187ec00e4. Error 8 sesudahnya akibat
channel fault. Penyebab stream invalid belum terkonfirmasi. Snapshot live
belum selesai karena aplikasi keluar. Build diagnostik kini menambahkan
HYDRA_PFIFO_TRACE=1: riwayat 64 word beserta state decoder dan pembandingan
isi memori saat fault. Build dan tes PFIFO dengan trace lulus. Prioritas
berikutnya: pengguna memilih CTR lagi, lalu periksa riwayat fault sebelum
mengubah aturan eksekusi atau mengklaim masalah frame sudah selesai.
Rincian dan referensi ada di [README PFIFO](../../tests/README-pfifo.md).

1. GPFIFO conditional fetch dan instruksi subdevice mask masih fault
   eksplisit. Level MAIN/SUBROUTINE sudah dieksekusi; pelaporan USERD
   TOP_LEVEL_GET belum diimplementasikan.
2. Flags submit dan ring/allocation lengkap. SyncFence,
   skip-refcounting, dan flag unknown ditolak; HwFormat native sudah diterima. SuppressWfi memakai ordering
   lebih kuat. Reservasi increment guest bit 8 sudah ditangani; release/reuse
   counter dan cakupan flag NVN lainnya belum lengkap.
3. AS_GPU binding dan isolasi per-AS sudah diimplementasikan dengan tes terarah;
   initialization/reservation/free/unmap, ownership handle nvmap dan pinning
   backing saat live remap masih terbuka. Lihat checkpoint binding di akhir.
4. Notifier mengikuti ABI Switch enable/disable tanpa mapping buffer guest.
   Pendekatan mapping Linux sebelumnya sudah dihapus. Recovery/reset channel
   fault belum tersedia; perlu channel baru. Koreksi jalur ioctl1 appended entries
   sudah diterapkan; rincian checkpoint input submit ada di akhir dokumen.
5. Semaphore reduction dan word semaphore yang terbelah antar host mapping.
   Klasifikasi error Metal menjadi error guest masih perlu bukti lebih rinci.
6. Live unmap/remap dan pinning seluruh backing allocation. Jaminan cleanup
   proses yang sudah diuji belum mencakup semua mutasi memori saat berjalan.
7. Validasi gameplay/hardware trace, pengukuran performa, dan audit akhir
   exit gate tahap instruksi.

Periksa juga backlog grafis lama untuk geometry/tessellation, advanced blend,
shader, format texture, dan presentasi. Perbaikan sesi ini tidak menutup area
tersebut. Daftar lama berisi temuan historis; cocokkan dengan kode dan dokumen
lanjutan sebelum menganggap suatu bug masih aktif atau sudah selesai.

## Cara melanjutkan pada sesi berikutnya

Daftar referensi yang diberikan pengguna tersimpan di [referensi.md](referensi.md).

1. Baca dokumen ini, aturan di [README](README.md), lalu
   [README PFIFO](../../tests/README-pfifo.md).
2. Periksa `git status`, perubahan terbaru, dan kode saat ini. Saat checkpoint
   dokumentasi dimulai, working tree bersih dengan HEAD `3f1b5ad5`
   (`docs: add README for PFIFO instruction stage detailing current status and validation`).
   Ini penanda riwayat, bukan perintah untuk reset atau checkout.
3. Tetap pada tahap A. Laporan 0 FPS/no-display telah membaik hingga CTR bisa
   masuk race menurut pengguna; lihat checkpoint terbaru di akhir dokumen.
   Binding/per-AS isolation sudah mendapat implementasi dan tes terarah.
   Prioritas berikutnya: lifecycle reservation/free/unmap serta live mapping/pinning,
   karena memengaruhi lifetime backing dan pekerjaan GPU tertunda. Gate ini belum
   ditutup oleh tes binding.
4. Pilih satu gate, telusuri referensi primer serta caller-nya, lakukan
   perbaikan satu keluarga perilaku, lalu jalankan tes yang relevan.
5. Perbarui bukti dan batasan di dokumen ini/README PFIFO. Jangan menandai
   tahap selesai hanya karena tes terbaru lulus.

Aturan kerja yang harus dipertahankan: gunakan `-j 6` pada mesin M1 8 GB;
rujuk NVIDIA untuk semantik guest dan Apple/SDK/Metal validation untuk host;
jangan menganggap emulator lain sebagai bukti tunggal. Jangan commit/stage
tanpa permintaan pengguna, jangan hapus tooling debug yang masih dipakai,
dan jangan menjalankan game atau mengirim input game; pengguna melakukan
pengujian gameplay. Pengguna kemudian memberi izin membuka/membuka ulang
aplikasi Hydra agar ia sendiri memilih game.

Prompt singkat untuk melanjutkan:

> Lanjutkan pekerjaan grafis Hydra dari
> docs/graphics-handoff/tujuan-dan-progress-2026-10-18.md dan tests/README-pfifo.md.
> Kita masih di tahap instruksi, status OPEN. Periksa kondisi repo terbaru,
> tuntaskan gate tahap ini beserta bukti tesnya sebelum berpindah tahap.

## Checkpoint terbaru: wait event dan koreksi ABI

Percobaan dengan riwayat 64 word tetap menghasilkan fault binding 0x40.
Word yang dibaca sama saat diperiksa ulang; itu belum membuktikan isinya benar
sejak submit. Ditemukan bug yang terpisah dan konkret pada status async wait:
pending wait mengembalikan BadValue (11), sedangkan caller libnx baru menunggu
kernel event pada Timeout (5). Status ini dikoreksi; EventSignal/cancel dan
perlindungan callback lama saat slot dipakai ulang juga ditambahkan.

Registrasi notifier sebelumnya perlu dikoreksi: semantik mapping driver Linux
berbeda dari ABI Switch. Implementasi Mach alias dihapus. Switch memakai
mem nonzero untuk enable, nol untuk disable, dan mengambil error lewat ioctl;
offset/size tidak memerlukan backing allocation. Referensi caller libnx dan
catatan Switchbrew tercantum dalam README PFIFO.

Build, tujuh pemeriksaan integrasi, empat suite regresi dan full shutdown lulus
dengan Metal validation. **Frame bolak-balik setelah koreksi wait/ABI belum
diverifikasi pengguna.** Berikutnya: pengguna memilih CTR di build yang dibuka
ulang, lalu periksa log apakah pending wait kini memakai result 5, counter
bergerak, dan apakah stream invalid masih terjadi. Tetap di tahap instruksi;
jangan menganggap perbaikan ini sudah membuktikan penyebab akhir gejala visual.

## Checkpoint: CTR masuk race; warning didahulukan

Pengguna kemudian mengonfirmasi gambar normal dan CTR bisa masuk hingga race.
Ini menggantikan status verifikasi pengguna pada checkpoint sebelumnya.
Hubungan langsung antara skipped wait dan reuse command buffer masih hipotesis;
hasil gameplay tidak menutup seluruh gate tahap instruksi.

Pengguna melaporkan FPS turun dibanding sebelumnya, tetapi pengukuran ulang
belum dilakukan. Permintaan terbaru: perbaiki penyebab warning terlebih dahulu.
Log `Hydra_2026-10-18_08-44-25.905922.log` memuat 51.546 warning
`Texture type mismatch (TIC: 2D, shader: cube)` dalam sekitar 168 detik.
Descriptor yang dilaporkan antara lain RT 640x360 dan 427x240, serta 6x3.
Belum terbukti apakah decoding shader, binding, atau resource pada cabang
yang tidak dieksekusi menjadi penyebabnya. Jangan memaksa texture 2D menjadi
cube atau menyatakan masalah selesai hanya dengan membatasi log.

Build diagnostik membatasi laporan mismatch ke 16, menambahkan konteks
shader/binding pada mismatch pertama, dan membatasi log informational async
wait. `HYDRA_LOG_TEXTURE_MISMATCHES` mengaktifkan seluruh laporan kembali.
Instrumentasi checkpoint Metal dengan `HYDRA_PERF_STATS` juga disiapkan;
ini belum memberikan bukti penyebab penurunan FPS. Dump shader berikutnya
disiapkan di `/tmp/hydra-texture-warning-2026-10-18/shaders` melalui
`HYDRA_DUMP_SHADERS`. Pengguna tetap memilih dan menjalankan game.

## Perapihan perbaikan no-display

Permintaan pengguna berikutnya adalah merapikan perbaikan sebelum melanjutkan
diagnosis warning. Perapihan ini mempertahankan perilaku yang telah membuat CTR
bisa masuk race, dengan pembagian tanggung jawab berikut:

- `NvHostEvent::armWait/cancelWait` mengelola token, retained event, dan generasi
  callback. Handler ioctl menangani validasi dan hasil ABI. Seluruh operasi slot
  diserialisasi oleh controller; callback memakai mutex state sendiri.
- Query dan cancel memakai decoder slot event yang sama. Pending async wait tetap
  mengembalikan Timeout, sesuai jalur tunggu event pada caller libnx. Log stub
  untuk handler wait yang sudah terimplementasi diganti log debug.
- Argumen SET_ERROR_NOTIFIER dinyatakan sebagai struktur 24 byte dengan pemeriksaan
  offset `mem` pada byte 16. Override `setErrorNotifier` pada ChannelBase dipakai
  kembali; parsing dan validasi input dilakukan sebelum perubahan state.
- Increment syncpoint 3D dan puller memakai `IRenderer::synchronize`, yang
  mengembalikan keberhasilan checkpoint. Engine tidak lagi mengimpor atau
  melakukan cast ke command buffer Metal untuk jalur ini. Kegagalan checkpoint
  tetap mem-fault channel dan tidak menerbitkan penyelesaian palsu.

Acuan ABI: [libnx fence.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/fence.c)
dan [nvchannel.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/ioctl/nvchannel.c).
Field increment 3D mengacu pada
[NVIDIA clb197.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h).
Menunggu seluruh kerja GPU untuk kedua CONDITION masih merupakan implementasi
konservatif; belum mengemulasikan waktu penyelesaian STREAM_OUT/ROP secara terpisah
dan belum ada kesimpulan tentang dampak FPS.

Validasi sesudah refactor: build app + shutdown-tests + gpu-engine-tests berhasil;
tujuh pemeriksaan integrasi (video-events, pfifo-decode, gpfifo-input,
channel-faults, channel-scheduling, gpu-process-cancel, semaphore-shutdown),
suite gpu-engine dan full shutdown semuanya exit 0 dengan Metal validation.
Log `/tmp/hydra-no-display-refactor-*.log`; `git diff --check` bersih.
Linker masih memperingatkan duplicate libc++ serta minimum macOS dependency
Homebrew fmt/SDL yang lebih baru daripada target app. Gameplay sesudah refactor
belum diuji pengguna. Warning texture dan gate instruksi lain tetap OPEN;
diagnostik opsional tidak dianggap perbaikan penyebab warning.

## Kembali ke tahap A: koreksi input submit Switch

Pengguna mengarahkan pekerjaan kembali ke urutan instruksi sampai gameplay.
Gate konkret pertama yang ditangani adalah perbedaan ioctl1 Linux dan Switch.
Caller libnx mengirim array entry setelah header IPC dan mengabaikan field
pointer `gpfifo`; Hydra sebelumnya justru membaca pointer CPU guest jika nonzero.
Jalur pembacaan pointer dihapus. Semua input ioctl1 memakai appended entries,
sedangkan ioctl2 tetap memakai inline buffer terpisah. Array IPC terpotong tidak
boleh diselamatkan dengan membaca pointer. Sumber primer: libnx nvchannel.c
yang ditautkan pada checkpoint sebelumnya dan README PFIFO.

Tes membedakan isi pointer guest dari payload IPC, menguji pointer nol, mapped
lintas halaman, partially unmapped dan overflowing, serta semua panjang array
terpotong. Ini koreksi ABI input umum; bukan klaim penyebab no-display CTR
sebelumnya, karena submit CTR yang diamati memakai ioctl2.

Build aplikasi dan shutdown-tests berhasil. Tujuh pemeriksaan integrasi tahap
instruksi serta full shutdown lulus dengan Metal validation dan PFIFO tracing.
Log `/tmp/hydra-switch-submit-*.log`; `git diff --check` bersih. Warning linker
dependency Homebrew yang tercatat sebelumnya masih ada. Gameplay sesudah
koreksi input ini belum diuji pengguna; warning texture tetap tercatat OPEN.

Tahap A tetap OPEN. Gate berikutnya: audit binding channel/address space AS_GPU,
kemudian lifecycle mapping dan backing memori; fitur packet/flags yang masih
unsupported juga tetap harus dituntaskan sebelum penutupan tahap.

## Audit lanjutan: batas mapping AS_GPU

Pengguna menegaskan tahap sebelumnya wajib dibereskan sebelum maju. Audit
menemukan bindChannel masih stub dan seluruh AS memakai GMMU milik proses;
isolasi AS belum dapat dinyatakan benar. Audit juga menemukan bug konkret pada
input mapping: offset/size dapat melewati nvmap, backing CPU berlubang tidak
divalidasi seluruhnya, dan remap menerapkan entry awal sebelum menolak entry
invalid di belakang. Remap memakai ukuran seluruh stream alih-alih sisa pada
cursor serta membaca array typed yang bisa tidak aligned.

Perbaikan: batas nvmap dan semua halaman backing diperiksa sebelum mapping;
alamat fixed yang overflow ditolak; MapBufferEx memeriksa input/output 40 byte
dan memakai scratch aligned; output baru disalin saat berhasil. Remap menyalin
descriptor dari cursor IPC dan memvalidasi seluruh batch sebelum menjalankan
entry pertama. Ukuran mapping nol tetap berarti seluruh nvmap, tanpa memperluas
range melewati ujung alokasi ketika buffer_offset nonzero.

Tes baru `--check-gpu-address-space` lulus. Build app/test, delapan pemeriksaan
integrasi instruksi, full shutdown, GPU-engine dan texture-cache lulus dengan
Metal validation. Log `/tmp/hydra-as-mapping-*.log`; rincian referensi dan cakupan
ada di README PFIFO. Kode hasil error defensif belum diverifikasi terhadap
hardware Switch untuk setiap invalid input.

**Tahap A tetap OPEN.** Binding/per-AS isolation, freeSpace dan pelepasan mapping
unmapBuffer yang sebenarnya, lifetime/pinning backing ketika remap/free berjalan
bersamaan dengan GPU, serta gates packet/flags sebelumnya masih perlu pekerjaan.
Validasi batch belum merupakan transaksi terhadap mutasi concurrent atau
kegagalan alokasi. Gameplay sesudah perubahan ini belum diuji pengguna.

## Binding channel dan address space per FD

BindChannel sekarang mencari FD target melalui session nvdrv dan memanggil
capability binding GPU; device lain ditolak tanpa RTTI. Setiap FD AS_GPU memiliki
GMMU sendiri dan diikat ke proses caller pertama. Channel dapat diikat sekali,
beberapa channel boleh berbagi AS, dan submit wajib memakai AS yang terikat.
Tidak ada fallback submit ke GMMU global proses. Mapping, reservation dan remap
diarahkan ke GMMU milik FD AS tersebut.

State PFIFO menahan AS dengan strong reference; pekerjaan host yang sudah
di-encode juga menahan state sampai completion, termasuk jalur yield/fault.
Proses menyimpan registry weak reference. Saat cleanup, seluruh AS dibatalkan
sebelum antreannya dihentikan dan GPU di-drain; backing CPU baru dilepas setelah
semuanya selesai. Registry weak tidak memperpanjang lifetime AS yang sudah
ditutup dan tidak punya pemilik pekerjaan lagi.

Dua perbaikan pendukung: cache lookup MMU memakai identitas objek selain pointer
dan generasi, agar reuse alamat host tidak menghidupkan cache lama; cache sumber
layer geometry dipindah dari global static menjadi state ThreeD tiap channel.
Tes caller proses lain juga membuka bug kernel: array 32 slot applet sebelumnya
hanya menandai slot pertama bebas. Constructor sekarang mengisi seluruh slot
dengan true. Alokasi dan pelepasan slot kedua teruji pada tes ownership.

Dasar: layout bind dari libnx, model single-bind dan reference lifetime dari
driver NVIDIA as_gk20a.c; tautan dan batas verifikasi ada di README PFIFO.
Kode error invalid-call yang dipilih masih kebijakan validasi Hydra dan belum
dibandingkan seluruhnya dengan trace hardware Switch.

Gate berikutnya tetap lifecycle reservation/free/unmap dan pinning backing;
ownership global handle nvmap serta fitur packet/flag sebelumnya juga belum
ditutup. Gameplay sesudah perubahan binding belum diuji pengguna.

Validasi final: build app dan target tes berhasil; sembilan pemeriksaan integrasi
instruksi, GPU-engine, texture-cache, shader-library, raster-state dan full
shutdown lulus dengan Metal validation. Tes lookup/cache MMU serta GPU memory
scattered backing/DMA juga lulus. Tes GPU memory memerlukan resource default
avatar, sehingga dijalankan melalui bundle sementara; dua fixture DMA lama
diperbaiki agar meminta transfer NON_PIPELINED, bukan NONE. Implementasi DMA
tidak diubah untuk meloloskan fixture. Rincian sumber dan log di README PFIFO.
Log final `/tmp/hydra-as-binding-final-*.log`; `git diff --check` bersih.
Modul hot-reload dibangun ulang untuk layout state baru; tes loading modul
production, output Metal, ABI rejection dan rollback juga lulus
(`/tmp/hydra-as-binding-hot-reload.log`).

## Lifecycle reservasi AS_GPU (sesi lanjutan)

AllocSpace kini mencatat reservasi, FreeSpace dan UnmapBuffer benar-benar
melepas mapping, penempatan non-fixed memakai first-fit yang menghindari
reservasi/mapping lain, dan blok nol sparse dimiliki GMMU (sebelumnya `calloc`
bocor). Semantik mengikuti gk20a `alloc_space/free_space/unmap_user`; kode error
adalah kebijakan Hydra. Reservasi non-sparse tidak lagi terbaca sebagai nol:
sesuai driver, tetapi bisa memunculkan fault pada game yang bergantung pada
perilaku lama. **Gameplay belum diuji pengguna.** Tes baru
`--check-gpu-as-lifecycle`; seluruh selector, suite engine/texture/raster, full
shutdown, tes lookup MMU lulus; `hydra-hot` dibangun ulang. Rincian dan batas di
[README PFIFO](../../tests/README-pfifo.md).

## Penuntasan gate tahap A (sesi lanjutan)

- **Pinning heap:** `resizeHeap` kini menyalin isi, mengarahkan ulang mapping
  semua GMMU, menginvalidasi cache, dan menahan blok lama sampai proses selesai.
  Batas: tulisan GPU yang terjadi setelah penyalinan tetapi sebelum repoint
  hilang (tanpa quiesce, karena menunggu PFIFO dapat deadlock). Jalur SVC unmap
  lain hanya mengubah page table sehingga tidak menggantung.
- **Subdevice mask dan conditional fetch** diimplementasikan menurut
  dev_pbdma.ref (ID subdevice 1). END_SUBDEVICE_MASK tetap fault; TOP_LEVEL_GET
  tidak diekspos.
- **Semaphore reduction** (MIN/MAX/XOR/AND/OR/ADD) dan report 16 byte yang
  terbelah antar mapping; INC/DEC ditolak karena aturannya tidak ada di header.
- **Syncpoint** kini didaur ulang saat channel/FD hancur (sebelumnya 191 slot
  tidak pernah dilepas); counter yang dipakai ulang melanjutkan nilainya.
- SyncFence tetap ditolak (fd Android sync tidak ada di Switch);
  SkipBufferRefcounting tetap ditolak karena belum ada sumber primernya.

Masih terbuka: END_SUBDEVICE_MASK, INC/DEC reduction, recovery channel setelah
fault (perlu channel baru), SharedMemory yang dilepas saat ter-map GPU, ownership
nvmap lintas proses, klasifikasi error Metal, validasi hardware/gameplay dan
performa. Tahap A tetap **OPEN**. Gameplay belum diuji pengguna.

## Audit referensi Switchbrew

Setelah daftar [referensi.md](referensi.md) dibaca, semantik AS_GPU/syncpoint
dibandingkan dengan Switchbrew. Hasilnya: BIND_CHANNEL cocok; sisanya adalah
asumsi dari Linux atau kebijakan Hydra. Pemeriksaan size aligned 0x1000 pada MAP_BUFFER_EX
sempat ditambahkan lalu DICABUT: game (CTR) gagal init karena InvalidSize. Masih terbuka: atribut
DeviceShared pada memori yang ter-map GPU, batas heap 4GB (0xCA01), flag
skip_buffer_refcounting, getVaRegions/Modify. Tes AS dan full shutdown lulus;
gameplay belum diuji.

## Status akhir sesi: ownership nvmap dan batas heap

Handle nvmap kini punya owner (proses pembuat) dan tabelnya diserialisasi;
MapBufferEx/Remap menolak handle proses lain. SetHeapSize menolak ukuran di
atas 4GB. Seluruh selector instruksi, suite engine/texture/raster, full shutdown
dan tes MMU lulus dengan Metal validation. Daftar gate yang selesai, yang butuh
sumber, dan yang ditunda (beserta alasannya) ada di bagian akhir
[README PFIFO](../../tests/README-pfifo.md). Tahap A tetap OPEN sampai gameplay
dan validasi hardware tersedia; berikutnya pengguna menguji game.

## Verifikasi gameplay setelah perubahan lifecycle

Pengguna menguji CTR: sempat 0 FPS karena batas nvmap yang terlalu ketat pada
MapBufferEx (NVN me-map big page penuh dari nvmap yang lebih kecil); setelah
batas dibulatkan ke big page, game terbuka dan masuk race. Log: tidak ada
kegagalan ioctl, fault PFIFO, atau fault MMU; sisa warning hanya texture
mismatch (terbatas 16) dan file save yang belum ada. Perf saat race 28-42 FPS
(encode CPU PFIFO 24-28 ms/frame, GPU Metal 13-14 ms, 1000+ draw/frame), menu
~60 FPS; belum dibandingkan dengan build lama. Ini bukti satu game, bukan
penutupan tahap A. Berikutnya: warning texture mismatch (cube vs 2D) dan
pengukuran FPS terhadap build pembanding; tes nvmap lebih kecil dari big page
sudah ditambahkan ke `--check-gpu-as-lifecycle` (terbukti gagal jika batas ketat lama dikembalikan).

## Pembersihan log W/E (CTR)

Log CTR sebelumnya memuat 19 jenis W. Perbaikan nyata: SET_TIMESLICE (ioctl
0x48/0x1d, Switchbrew), puller MEM_OP_A..D/NON_STALL_INTERRUPT/FB_FLUSH
(clb06f.h; MEMBAR menunggu GPU, operasi TLB/L2 no-op karena tanpa TLB dan cache
host koheren), underflow `ukuran - offset` pada IFile/IStorage::read, dan crop
kosong layer (= seluruh buffer, semantik BufferQueue). Texture yang tipenya tidak
bisa dipetakan ke tipe shader (mis. TIC 2D vs shader cube) tidak lagi di-bind
(hardware: tidak terdefinisi; Metal tidak bisa membaca lewat binding bertipe
lain); TIC nol dianggap null. Diturunkan ke debug dengan alasan: file save tidak
ada, IService::close, GetDisplayIDFromName, GetRelayService, ConvertScalingMode
(stub, pemetaan tidak terdokumentasi), revisi npad, strap guide, CoreMask
(Hydra melapor 4 core). Hasil: sesi CTR ~1m50 (6.537 baris) tanpa satu pun W/E/F.
PERF 43-52 FPS di scene yang diuji (sebelumnya 28-42 di scene berbeda; tidak
dibandingkan apple-to-apple). Perlu dicek visual: refleksi/langit setelah
perubahan binding texture bertipe tidak cocok.

## CTR: jejak ban dan glow ban saat drift/nitro (investigasi, 18 Oktober 2026)

**Jejak ban hitam = benar.** Pengguna membandingkan dengan game asli. Shader
decal `0xa60030` (deferred decal dari depth z32float + BC3 128x128, alpha blend)
memang memakai tint (0,0,0) dari constant buffer game. Dugaan "regresi karena
tekstur tipe tidak cocok tidak di-bind" dibantah: jejak tetap hitam dengan
`HYDRA_BIND_MISMATCHED_TEXTURES=1`. Saklar itu tetap ada untuk perbandingan.

**Glow ban/nitro putih (OPEN).** Gambar referensi pengguna (game asli): roda
bercincin glow oranye/kuning, debu cokelat, asap gelap. Di Hydra efeknya ada
tetapi **putih**, tanpa warna. 14 screenshot per detik + trace frame ada di
`/tmp/hydra-boost/{shots,trace,shaders}` (sementara). Temuan:
- Api knalpot tampak oranye/merah (wajar); asap roda putih keabu-abuan saat drift.
- Trace frame_6 (17:55:35) memuat partikel aditif `0x4850030` dengan vertex
  color biru-ungu (93,0,255,0), blend ONE/ONE_MINUS_SRC_ALPHA, BC3 128x128 putih,
  soft-particle memakai depth r32float. Data warna dikirim game; belum terbukti
  bahwa ini partikel yang sama dengan glow ban.
- Dari MSL hasil dump: warna FS `0x4850030` = (tex.rgb * varying 0x80..0x88 *
  fade + varying 0xa0..0xa8 * alpha) * faktor; varying berasal dari atribut vertex
  melalui VS `0x4530030`. Jalur ini belum diverifikasi numerik. Kandidat penyebab
  (hipotesis): warna varying hilang (atribut unorm8x4, `size=0xa type=2`, atau
  mapping VS->FS), tint dari constant buffer terbaca salah/usang, atau lapisan
  glow aditif jenuh ke putih.
- Tracer hanya mencetak 40 nilai pertama tiap constant buffer; perlu dump penuh.
- Draw `0x26b0030` yang sempat dicurigai ternyata sprite HUD (ortografik).

**Langkah berikutnya:** capture piksel keluaran draw partikel
(`tools/visual_debug.py --directory <trace>/visual capture --shader 0x4850030`;
permintaan masih antre di `/tmp/hydra-boost/trace/visual/request`), lalu
bandingkan warna keluaran dengan vertex color; tambah dump penuh constant buffer
di tracer; bila perlu, harness offline yang menjalankan VS+FS dump dengan input
terekam. Pendekatan ini mengikuti saran pengguna: periksa penerjemah (global),
bukan perilaku satu game. Belum ada perubahan kode untuk masalah ini.

Catatan alat: `LOG_DEBUG` dibuang di build release (jangan andalkan log debug);
laporan mismatch tekstur kini INFO (maks 32). Screenshot jendela:
`screencapture -l <windowID>` (ID via CoreGraphics), trace manual lewat file
`<HYDRA_TRACE_DIR>/trigger`.

## Performa: bottleneck encode PFIFO di CTR (18 Oktober 2026)

**Bukti (sampling `sample <pid> 8` pada thread PFIFO, di race, 700-1100 draw/frame):**
thread PFIFO sibuk ~99%; game CPU-bound pada satu thread. Metal GPU hanya 11-13
ms/frame, sedangkan encode CPU 19-21 ms/frame (60 FPS butuh <=16,6). Encode hampir
tidak bergantung jumlah draw (61 draw masih ~20 ms) sehingga biaya tetap per frame
(checkpoint tunggu-GPU, encoder) ikut besar. Komposisi (inklusif, % thread):

| Bagian | Sebelum | Sesudah |
|---|---|---|
| Alokasi MTLBuffer occlusion (`prepareOcclusionQuery`, panggilan kernel) | ~8% | pool (hilang dari profil) |
| `setReportSemaphore` menunggu GPU | ~8% | ~6% |
| `incrementSyncPoint` menunggu GPU | ~4% | <3% |
| `getShader` (2x per draw; hash+find ganda, baca TSC tiap texture) | ~10% | ~10% |
| Pembuatan render encoder (~67/frame) | ~4% | ~4% |
| `bindDrawState` (setBuffer/setTexture AGX) | ~15% | ~15% |

**Diperbaiki:** (1) halaman buffer occlusion dipakai ulang lewat pool (didaur ulang
hanya setelah checkpoint menunggu GPU); (2) `debugCommitAndWait` melewati
checkpoint bila command buffer belum berisi apa pun dan tidak ada submit lain yang
masih berjalan (penghitung commit/completion). Terukur: ~4,5 checkpoint/frame
dilewati, command buffer per frame 10,2 -> 5,7, FPS race ~46-55 -> ~50-52, encode
~20-21 -> ~19-20 ms. Perbandingan antar sesi tidak apple-to-apple (scene berbeda).
Statistik baru: baris `CHECKPOINTS skipped` pada `HYDRA_PERF_STATS=1`.

**Belum (opsi, perkiraan dampak dan risiko):**
- Report/syncpoint asinkron (tulis di completion handler dengan antrean berurutan):
  ~6-8%; risiko tinggi (ordering, epoch occlusion, render kondisional membaca hasil).
- Memoisasi `getShader`/`getPipeline`: ~8-10%; risiko sedang-tinggi karena shader
  bergantung pada konstanta (branch_constants), sampler, dan isi kode guest.
- Mengurangi jumlah render encoder (gabung pass) atau encode multi-thread: dampak
  besar, desain besar.
Setelah semua itu pun 60 FPS konsisten di scene terberat belum terjamin.

## Audit keluar tahap A dan penutupan item kecil (18 Oktober 2026)

**Tekstur 2D di slot cube (ditutup, dengan bukti dari shader).** Shader lighting
`0x7ef0030` (3.425 baris MSL hasil dump) memilih satu cabang lewat jump table
(`BRX`; nilai dibaca dari constant buffer c1) dan tiap cabang men-sample satu
cube probe refleksi: tex22/34/38/40. Binding 0x26 (host slot 11, tex38) hanya
dipakai di blok `label0x17b` (lookup environment map dengan level = 6 - 6*roughness).
Slot yang tidak dipilih menyimpan deskriptor lama (di sini RT 2D 640x360) dan
tidak pernah di-sample di hardware, sehingga tidak membawa dampak; tidak
mem-bind tekstur bertipe tidak cocok (perubahan sesi ini) aman. Batas: belum
diverifikasi bahwa cabang itu tidak terpilih pada piksel mana pun; jika terpilih
dengan deskriptor salah, hasilnya hitam (sebelumnya sampah) dan itu cacat data game.

**SharedMemory yang dilepas saat masih ter-map (tidak diubah, dianalisis).**
`SharedMemory::~SharedMemory` membebaskan memori host saat refcount handle nol,
walau page table guest (atau mapping GPU) masih menunjuk ke sana; Horizon asli
menahan memori sampai di-unmap. Memperbaikinya butuh penghitungan map/unmap per
rentang (UnmapSharedMemory menerima alamat/ukuran sembarang, bisa parsial) dan
pelepasan saat proses mati; perubahan refcount kernel itu rapuh dan dampaknya
nyata hanya bila guest menutup handle sebelum unmap (libnx `shmemClose` unmap
dulu). Dicatat sebagai risiko yang diketahui, bukan bug yang teramati.

**Status tahap A per gate:**

| Gate | Status |
|---|---|
| Input GPFIFO, decode PFIFO, isolasi channel, scheduling, semaphore/syncpoint, fault, lifetime proses | Diimplementasi dan diuji |
| AS_GPU: binding, reservasi/free/unmap, ownership nvmap, batas heap, pinning heap | Diimplementasi dan diuji |
| Subdevice mask, conditional fetch, reduction (kecuali INC/DEC), report terbelah, daur ulang syncpoint | Diimplementasi dan diuji |
| INC/DEC reduction, END_SUBDEVICE_MASK, skip_buffer_refcounting, kode error Switch, getVaRegions/Modify | **Diblokir: tidak ada sumber primer** |
| DeviceShared, FROM_ID per-klien, SharedMemory lifetime, recovery channel | Ditunda dengan alasan (tanpa gejala teramati) |
| Validasi hardware/trace dan gameplay luas | Belum ada |

Tahap A tidak ditandai tertutup. Item yang tersisa tidak dapat diselesaikan dengan
bukti yang tersedia atau tidak punya gejala; keputusan memindahkan fokus ke tahap
B-D (mulai dari glow ban putih) menunggu persetujuan pengguna sesuai aturan
"jangan pindah tahap sebelum tahap sebelumnya tuntas".

## Tahap B/C: audit blend terhadap header NVIDIA (18 Oktober 2026)

Acuan primer: [NVIDIA clb197.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h)
(`SET_BLEND_COLOR_OP/SOURCE_COEFF/DEST_COEFF`, `SET_BLEND_ALPHA_*`,
`SET_BLEND_SEPARATE_FOR_ALPHA`, `SET_BLEND_PER_TARGET_*`).

- **Cocok persis:** nilai operasi (OGL 0x8006-0x800B, D3D 1-5) dan faktor (OGL
  0x4000/0x4001/0x4300-0x4308/0xC001-0xC004/0xC900-0xC903; D3D 1-0x13) dengan
  `getBlendOperation`/`getBlendFactor`. Offset struktur global (0x133c-0x1358)
  dan per target (0x1e00 + j*32) cocok dengan `Regs3D`.
- **Cacat ditemukan dan diperbaiki:** `SET_BLEND_SEPARATE_FOR_ALPHA` (global dan per
  target) tidak pernah dibaca; persamaan alpha selalu memakai register alpha
  walau flag nol. Kini `getBlendEquation` memakai persamaan warna untuk alpha bila
  bit 0 nol (juga untuk deteksi dual-source). Header hanya menamai field; semantik
  "alpha mengikuti warna" mengikuti nama register dan perilaku glBlendFunc
  versus glBlendFuncSeparate, bukan penjelasan eksplisit NVIDIA. Field per target
  yang bernama `unknown` diganti `separate_for_alpha`; trace blend memuat `sep=`.
- Tes `testBlendSeparateForAlpha` (gpu-engine): flag nol, satu, bit lain, dan
  per target. gpu-engine, texture-cache, raster-state, full shutdown lulus.
  Dampak visual belum diuji pengguna; draw blend CTR yang terlihat di trace
  (`rgb 0x1(0x2,0x6) a 0x1(0x2,0x6)`) sudah sama sehingga tidak berubah, tetapi
  draw dengan alpha berbeda (mis. `a 0x3(0x2,0x2)`) bisa berubah jika flag-nya nol.

## Tahap B: audit tata letak register, deskriptor, dan enum terhadap header NVIDIA

Acuan primer (disalin ke scratchpad sesi, bukan fixture):
[clb197.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h) dan
[clb197tex.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197tex.h).
Tiga tes baru di `hydra-gpu-engine-tests` memeriksa kecocokan secara otomatis; satu
mutasi (menggeser `address_v` satu bit) terbukti membuat tes gagal.

| Area | Hasil |
|---|---|
| `Regs3D` (`testRegisterLayout`) | 61 register dan 10 stride array cocok dengan offset method header (viewport, scissor, depth/stencil target, atribut vertex, blend, index buffer, report, vertex stream, shader program, constant buffer, bindless) |
| TIC block-linear (`testTextureDescriptorLayout`) | 53 field cocok dengan `TEXHEAD_BL_*` pada seluruh 256 bit; tiap field diuji sendiri dan tidak boleh menumpang field lain |
| TSC | 26 field cocok dengan `TEXSAMP0-7` |
| Atribut vertex dan render target (`testVertexAttributeAndRenderTargetLayout`) | `SET_VERTEX_ATTRIBUTE_A` (stream, source, offset 14 bit, lebar komponen 6 bit, tipe numerik, swap R/B), nilai enum ukuran dan tipe; blok `SET_COLOR_TARGET_*` (offset dan field `MEMORY`) cocok |
| Enum nilai | `ImageFormat` (semua 77 nilai `COMPONENTS`), tipe komponen dan sumber swizzle TIC, `PrimitiveType` (0-0xE), `TextureType` (0-8), versi header TIC (0-4), jenis filter/mip/anisotropi/alamat sampler, depth compare (D3D 1-8 dan OGL 0x200-0x207), faktor dan operasi blend: cocok |
| Format | `DepthSurfaceFormat`: seluruh 11 format header ada. `ColorSurfaceFormat`: 8 format header belum ada di enum (0x40-0x47: A16, AF16, AF32, A8R8, R16_A16, RF16_AF16, RF32_AF32, B8G8R8A8); sudah tercatat tidak didukung (draw ke target itu dilewati, bukan abort) |

Satu-satunya cacat perilaku yang ditemukan pada audit ini adalah flag
`SEPARATE_FOR_ALPHA` (diperbaiki, bagian sebelumnya). Batas: ini audit layout
dan nilai, bukan bukti bahwa tiap field dipakai dengan semantik yang benar di
seluruh engine. Belum diaudit: semantik stencil/depth state, nilai format TIC
penuh (`COMPONENTS` + tipe data) terhadap tabel format Hydra, operasi draw/
dispatch, DMA/2D/inline. Gameplay belum diuji pengguna.

## Tahap B: penuntasan audit engine dan resource (18 Oktober 2026)

Acuan primer tambahan (NVIDIA open-gpu-doc, disalin ke scratchpad sesi):
[clb1c0qmd.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/compute/clb1c0qmd.h),
[clb1c0.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/compute/clb1c0.h),
[clb0b5.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/dma-copy/clb0b5.h),
[cl902d.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/twod/cl902d.h),
[cla140.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/inline-to-memory/cla140.h).

| Area | Hasil | Bukti |
|---|---|---|
| Tabel method engine 3D (21 entri: draw, clear, report, MME, constant buffer, bind group, inline index, increment syncpoint) | Nama dan offset cocok dengan header | skrip audit |
| Indeks register mentah `regs_raw[...]` di `3d.cpp` (stencil depan/belakang, depth bias, clip control, alpha test, user clip, point, render enable, report) | Semua cocok dengan nama register header | skrip audit |
| Stencil/cull/depth bias | Nilai enum (OGL stencil op/func, 0x404/0x405/0x408, 0x900/0x901) dan offset cocok | skrip audit |
| `BIND_GROUP_CONSTANT_BUFFER` | VALID bit 0, SHADER_SLOT 8:4, stride 8 word per grup cocok | pembacaan kode + header |
| QMD compute (V01_07) | 26 field cocok. Dua pelebaran mask mengikuti header: grid width 32 bit (sebelumnya 31) dan `SHADER_LOCAL_MEMORY_LOW_SIZE` 24 bit (sebelumnya 20; accessor tidak dipakai) | `testComputeQmdLayout` |
| Register compute (SEND_PCAS_A, pool TSC/TIC, program region, bindless) | Cocok | `testComputeQmdLayout` |
| DMA copy (register, `SET_REMAP_COMPONENTS`, block size, origin; bit `LAUNCH_DMA` diperiksa manual karena struct privat) | Cocok | `testCopyEngineLayout` |
| 2D (`cl902d.h`: dst/src, clip, color key, ROP, operasi, `PIXELS_FROM_MEMORY`, sample mode) | Cocok | `testTwoDEngineLayout` |
| Inline-to-memory (`cla140.h`) | Cocok | `testInlineEngineLayout` |

Ditambah tiga audit sebelumnya (layout `Regs3D`, TIC/TSC, atribut vertex/render
target, format, blend) total tujuh tes layout di `hydra-gpu-engine-tests`.

**Satu-satunya cacat perilaku tahap B:** `SEPARATE_FOR_ALPHA` (sudah diperbaiki).

**Batas yang sengaja dicatat, bukan klaim selesai:**
- Layout dan nilai enum terbukti; semantik tiap field di seluruh engine hanya
  diuji sejauh suite yang ada (draw, clear, occlusion, DMA, 2D, texture cache,
  index, primitive restart, storage coherence) dan gameplay CTR.
- Pada checkpoint audit ini, constant buffer masih menyimpan pointer host saat
  bind. Celah remap tanpa bind ulang kemudian diperbaiki; lihat checkpoint
  "Constant buffer mengikuti mapping GPU" serta koreksi regresi buffer
  terpisah pada checkpoint berikutnya di akhir dokumen.
- Invalidasi shader cache oleh `INVALIDATE_SHADER_CACHES` dan oleh penulisan CPU ke
  kode shader belum ada (kunci cache hanya mencuplik 9 byte kode); ini masuk
  tahap C (shader dan pipeline).
- Delapan format render target header (0x40-0x47) belum didukung.
- `SET_FALCON04` (firmwareCall4) dan `WAIT_FOR_IDLE` kelas 3D hanya disimpan
  sebagai register; belum ada bukti ia diperlukan.

Tahap B dinyatakan **tuntas sejauh audit sumber primer yang tersedia**; bukan
bukti kebenaran semantik penuh atau kompatibilitas gameplay.

## Tes referensi blend dan format HDR (18 Oktober 2026)

Dibuat untuk menguji hipotesis bug putih (glow ban, baji cahaya air terjun):
target HDR terpotong ke 1.0 atau blend additive salah.

- `tests/blend_reference.py` menghitung hasil yang benar di Python murni:
  persamaan blend (add/sub/rsub/min/max, enam faktor), float16 IEEE, clamp
  unorm8, dan float tanpa tanda R11G11B10 (alpha dibaca 1).
- `tests/blend_reference_tests.cpp` (target `hydra-blend-reference-tests`)
  menggambar kasus yang sama lewat tabel format Maxwell ke Metal milik Hydra
  (`toMtlPixelFormat`) dan menulis byte mentah target. Python mendekode dan
  membandingkan.
- Jalankan: `python3 tests/blend_reference.py <path hydra-blend-reference-tests>`.

Hasil: **2160/2160 kasus cocok** (RGBA16Float, RG11B10Float, RGBA8Unorm; source
di atas 1.0 dan HDR; dengan `MTL_DEBUG_LAYER=1`). Nilai uji sengaja dijauhkan
dari titik bulat x.5/255 karena pembulatan tie berbeda 1 LSB antara Python dan
Metal.

Audit tambahan: seluruh konstanta op/faktor blend di `3d.cpp` (OGL dan D3D,
termasuk constant color 0xC001-0xC004 dan SRC1 0xC900-0xC903) cocok dengan
`clb197.h`.

**Yang dibuktikan:** format target dan blend dasar tidak memotong atau salah
menghitung HDR. **Yang tidak dibuktikan:** konversi enum lewat `getBlendFactor`
(fungsi privat, hanya diaudit teks), format lain (`RGBX16Float` dipetakan ke
RGBA16Float sebagai HACK: alpha tidak dibaca 1 seperti hardware), dan jalur
shader Maxwell ke MSL. Bug putih kini mengarah ke shader atau state spesifik
game; langkah berikutnya butuh capture satu frame.

## Constant buffer mengikuti mapping GPU (lanjutan 18 Oktober 2026)

Celah lifetime dari audit tahap B ditangani: `ThreeD::bindGroup` kini menyimpan
alamat GPU dan ukuran, bukan pointer host hasil lookup saat bind. Semua konsumen
binding 3D memakai `getConstBuffer`: upload uniform, descriptor storage,
texture handle, constant-buffer branch table shader, trace, dan visual capture.
Resolver memakai lookup GMMU yang sudah memiliki invalidasi generasi; tidak
mengalokasikan atau menyalin payload pada jalur ini.

Acuan layout tetap [NVIDIA clb197.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h):
`SET_CONSTANT_BUFFER_SELECTOR_A/B/C` (0x2380/84/88), ukuran 16:0, alamat
upper 7:0 dan lower 31:0; `BIND_GROUP_CONSTANT_BUFFER(j)` (0x2410 + j*32),
VALID bit 0 dan SHADER_SLOT 8:4. Header membuktikan field register; kebijakan
mengembalikan range kosong pada backing yang tidak dapat diakses adalah
perlindungan host Hydra, bukan klaim kode fault hardware terverifikasi.

`testConstBufferRemapping` di `hydra-gpu-engine-tests` memeriksa remap dan
unmap tanpa bind ulang, heap rebase, pergantian address space, isolasi stage/slot,
disable, ukuran nol, mapping parsial, serta rentang lintas mapping dengan
backing host contiguous. Data upload uniform sesudah remap diperiksa berbeda
dari snapshot sebelumnya; snapshot sebelumnya tetap utuh. Mutasi yang
mengembalikan penyimpanan pointer host saat bind membuat tes gagal pada
pemeriksaan mapping sesudah remap (exit 1), lalu perbaikan dipulihkan.

Validasi dengan `-j 6` dan `MTL_DEBUG_LAYER=1`: build aplikasi dan target tes,
suite GPU engine, shader-library, texture-cache, tes integrasi
`--check-gpu-heap-resize`, serta full shutdown lulus. Build tetap mengeluarkan
warning linker dependency Homebrew yang ditargetkan ke macOS lebih baru;
tidak ada error build. Aplikasi hasil build: `build/swiftui-release/bin/Hydra.app`.

**Batas:** backing host yang tidak contiguous belum bisa di-upload sebagai satu
uniform buffer; resolver menolaknya dengan warning eksplisit, bukan membaca
melewati mapping pertama. Dukungan scatter/gather penuh tetap OPEN. Lookup saat
konsumsi juga bukan jaminan pinning/locking untuk seluruh durasi draw ketika
mapping diubah secara bersamaan. Jalur compute mengambil binding dari QMD per
dispatch dan tidak diubah pada pekerjaan ini. Tidak ada gameplay yang dijalankan;
hubungan dengan glow putih belum terbukti. Invalidasi shader cache dan capture
frame glow masih merupakan pekerjaan terbuka berikutnya.

## Referensi Python: atribut vertex dan VS→FS (lanjutan 18 Oktober 2026)

Mengikuti saran pengguna, ditambahkan oracle Python independen untuk decoding
nilai atribut, normalisasi, komponen default, dan pertukaran R/B. Harness C++
memakai generator MSL dari IR serta pipeline Metal Hydra, lalu mengembalikan
byte piksel RGBA32Float ke Python. Encoding diperiksa terhadap 26 field/nilai
header NVIDIA `clb197.h`; persamaan normalisasi memakai referensi OpenGL,
hasil host diperiksa lewat Metal validation. Batas bukti ini dibedakan dari
hardware trace GM20B.

Ditemukan bug engine umum: hook backend menukar `__in` sesudah input disalin ke
register atribut guest. Swap lewat shader tidak berpengaruh; hanya swap native
Metal untuk UNORM8x4 yang bekerja. Hook sekarang dipanggil sebelum penyalinan.
Hasil sebelum: **1104/1601 cocok (497 mismatch)**; sesudah: **1601/1601 cocok**.
Suite shader-library dan GPU engine dengan Metal validation serta build aplikasi
lulus. Tes dimulai dari IR; decoding instruksi Maxwell belum dibuktikan oleh tes
ini. Tidak ada gameplay yang dijalankan.

Warna trace CTR `(93, 0, 255, 0)`, UNORM8x4 tanpa swap, menghasilkan
`(0.3647058904, 0, 1, 0)` sebelum dan sesudah perbaikan. Jadi warna tersebut
tidak hilang pada konversi atribut/varying yang diuji; belum membuktikan penyebab
glow putih atau keseluruhan shader partikel game.

Perintah reproduksi, cakupan format, sumber primer, dan batasan tersedia di
[README vertex reference](../../tests/README-vertex-reference.md).


## Regresi scene 3D hitam: constant buffer lintas mapping (18 Oktober 2026)

Pengguna menjalankan build terbaru dan melaporkan HUD CTR tampil, tetapi scene
3D hitam. Log `Hydra_2026-10-18_15-37-17.714817.log` menunjukkan penolakan
`Noncontiguous constant buffer at GPU 0x861600 is unsupported` pada detik 1,072,
kemudian 637.661 warning uniform tidak ter-bind selama sesi tersebut.
Penolakan itu berasal dari perubahan resolver pada sesi ini. Tes sebelumnya
justru mengharapkan penolakan buffer terpisah, sehingga belum cukup menjaga
kompatibilitas; batas yang dicatat ternyata digunakan oleh game.

Koreksi:
- Binding tetap menyimpan alamat GPU agar remap/rebase terlihat tanpa bind ulang.
- Backing contiguous memakai jalur cache yang ada. Backing terpisah dibaca dalam
  urutan alamat GPU ke scratch milik binding; seluruh rentang diperiksa, mapping
  berlubang tetap tidak dibaca. Ukuran binding memakai field SIZE 16:0 NVIDIA.
- Upload scratch memakai snapshot immutable, bukan cache berdasarkan alamat
  scratch (yang tidak memiliki guest write tracking). Snapshot hanya dipakai
  ulang bila seluruh byte masih sama; upload lama tetap hidup untuk draw yang
  telah di-encode. Pool snapshot menunggu completion GPU sebelum reuse.
- Konsumen CPU (konstanta branch, texture handle, descriptor storage, trace)
  memakai hasil gather yang sama. Tidak ada konversi isi atau aturan per game.

`testConstBufferRemapping` kini mengharuskan dukungan backing terpisah, bukan
penolakan. Tes memeriksa urutan kedua potongan, reuse byte-identik, perubahan CPU,
snapshot lama yang tidak berubah, lubang mapping, remap/rebase, dan mask ukuran.
Kernel Metal sungguhan membaca kedua snapshot melalui binding `constant uint*`
ke output host dan hasil seluruh word diperiksa. Buffer terpisah kini didukung
pada jalur ini; catatan unsupported di checkpoint sebelumnya bersifat historis.

Build aplikasi dan suite GPU engine, shader-library, full shutdown, tes heap
resize, serta **1601/1601 kasus Python vertex** lulus dengan Metal validation.
Tidak ada gameplay yang dijalankan oleh agen. **Pemulihan scene 3D masih perlu
verifikasi pengguna**, walaupun jalur penolakan yang terlihat di log telah
diganti dengan gather + upload yang diuji. Efek performa gather pada gameplay
belum diukur; pinning terhadap perubahan mapping bersamaan masih batas terpisah.

## Verifikasi pengguna: 3D pulih; efek putih dekat air terjun masih OPEN

Setelah koreksi buffer terpisah, pengguna mengirim dua screenshot CTR dengan
scene 3D kembali tampil. Ini menggantikan status pemulihan yang sebelumnya
menunggu verifikasi. Log terbaru `Hydra_2026-10-18_15-43-58.464195.log` memuat
4.873 baris tanpa W/E/F. Ketiadaan warning tidak membuktikan hasil visual benar.

Pengguna menduga refleksi air: screenshot menunjukkan highlight putih kuat dekat
air terjun serta lapisan kabut/cahaya di jalan dan sebagian kart. Belum diketahui
apakah kedua gambar sama-sama Hydra atau salah satunya referensi hardware asli.
Dari screenshot belum dapat ditentukan apakah sumbernya reflection/environment
sampling, bloom, atau partikel transparan/depth fade. Tidak ada perubahan shader
atau brightness berdasarkan dugaan ini. Capture draw scene tersebut belum ada;
proses Hydra sudah tidak berjalan saat pemeriksaan log. Pengguna tetap menjalankan
game dan mengatur kameranya sendiri.

## Audit Python 3D: depth/stencil numerik (18 Oktober 2026)

Pengguna meminta perhitungan Python lanjutan untuk mencari bug 3D. Ditambahkan
`depth_stencil_reference.py` dan harness Metal melalui renderer Hydra. Python
menghitung dua fragment berurutan; hasil RGBA, D32Float dan stencil 8-bit GPU
harus cocok persis. Shader tes memakai MSL sederhana agar persamaan fixed-function
terisolasi. Pipeline, binding uniform, dynamic reference, state cache, render pass
serta pergantian encoder tetap memakai kode produksi Hydra.

Hasil baru: **6656/6656** kasus cocok: depth 896, operasi stencil 2880,
perbandingan/mask stencil 2880. Delapan fungsi compare dan delapan operasi
stencil diuji dengan encoding OGL/D3D, fungsi enable/write depth, batas saturasi/
wrap, mask, perubahan reference dan render→blit→render. 32 enum dicocokkan langsung
dengan header NVIDIA; persamaan referensi memakai OpenGL 4.6 §17.3.3–17.3.4.
Tidak ada reference stencil di atas 255 pada kasus final; semantik register untuk
nilai tersebut tidak disimpulkan dari aturan clamp pada API GL.

Decoder operasi stencil diekstrak tanpa perubahan dari lambda `drawInternal`
menjadi helper `decodeStencilOp` agar harness memakai decoder produksi yang sama.
Mutasi sengaja increment-wrap menjadi increment-clamp menghasilkan **180 mismatch**
(6476/6656 cocok, exit 1). Decoder benar kemudian dipulihkan dan suite penuh
kembali 6656/6656. Tidak ditemukan bug perilaku depth/stencil baru dalam subset ini.

Build aplikasi dan target tes lulus (`-j 6`). GPU engine dan raster-state lulus
dengan Metal validation; oracle Python vertex **1601/1601** dan blend/HDR
**2160/2160** juga dijalankan kembali dan lulus. Tidak ada gameplay yang dijalankan
oleh agen, tidak ada commit/stage.

Batas: D32Float+S8 satu sample, stencil front/back sama, depth in-range. Pemilihan
register 3D, decoding/arithmetic shader Maxwell, depth D16/D24, depth bias/clipping,
MSAA, explicit fragment depth, sampling depth/reflection serta bloom belum dibuktikan
oleh tes baru. Efek putih dekat air terjun masih OPEN; hasil ini bukan sertifikasi
seluruh 3D. Petunjuk reproduksi dan sumber primer ada di
[README depth/stencil reference](../../tests/README-depth-stencil-reference.md).
