# Pengelolaan pekerjaan CPU dan GPU — 2026-10-20

Lanjutan [Resource Registry](resource-registry-2026-10-20.md).
Branch `arvino`; belum di-stage/commit. Tidak menjalankan GUI atau game.

## Alur yang aktif

Guest submission → admission budget → worker CPU PFIFO → resolved DrawCommand
+ validasi registry/mapping + ownership leases → encoding Metal → admission
host submission → GPU completion → publikasi report/fence → retirement.

`core/execution/WorkManager` adalah modul host untuk lifecycle pekerjaan. Executor
CPU yang sudah ada tetap memilih channel runnable dan melakukan decode; Metal
menjalankan command buffer dalam urutan commit. Manager tidak membuat worker
kedua atau memindahkan decode ke thread yang kehilangan TLS/lock engine.

Status ticket: Queued → Running → Succeeded/Failed/Cancelled. Hasil terminal
pertama menang; pekerjaan yang belum mulai tidak dapat dianggap sukses.
Dependency ticket menahan start sampai seluruh predecessor sukses; failure atau
cancellation predecessor menggagalkan dependent. Dependencies dilepas setelah
terminal, di luar mutex manager. Antrean dan sinkronisasi guest PFIFO yang sudah
ada tetap menjadi otoritas urutan antar-channel.

## Budget dan cancellation

- CPU dispatch PFIFO: maksimal 4.096 reservasi/list yang belum selesai dan
  total 64 MiB salinan **array entry GPFIFO**. Ini bukan batas seluruh guest
  pushbuffer, texture cache, atau seluruh memori emulator.
- Admission CPU nonblocking. Menunggu slot di syscall dapat menghalangi channel
  yang harus menghasilkan fence/semaphore bagi channel lain. Penolakan melalui
  ioctl menghasilkan `InsufficientMemory` sebelum reservasi fence.
- `PfifoAdmission` move-only mengikat reservasi ke satu PFIFO dan ukuran array.
  Reservasi yang tidak dipakai otomatis dibatalkan. Submission counter baru
  bertambah setelah list benar-benar masuk antrean.
- Budget dispatch ditahan saat decode/yield dan sampai callback completion
  terakhir (setelah report/fence), bukan hanya sampai CPU selesai encoding.
- PFIFO menyimpan weak references ticket per AS, termasuk setelah decode selesai
  dan native submission masih in flight. Pembatalan AS membatalkan semua ticket
  tersebut; callback yang datang kemudian tidak mempublikasikan guest success.
  Native GPU work tetap di-drain sebelum guest allocation dilepas.
- Pembatalan AS membatalkan queued tickets; teardown PFIFO menutup admission,
  membatalkan antrean dan menunggu worker seperti sebelumnya. Running host work
  tetap mengikuti mekanisme drain GPU/report yang sudah ada.
- GPU Metal: maksimal 64 command buffer yang sedang in flight per shared
  CompletionQueue. Submit berikutnya menunggu slot; callback native melepaskan
  slot pada sukses **atau** error. Checkpoint dan destructor commit memakai
  jalur admission yang sama, termasuk commit profiling compute. Satu-satunya
  native commit berada di fungsi manager host. Ini batas jumlah submission,
  bukan budget byte GPU.
- Standalone command buffers tanpa CompletionQueue memakai fallback manager
  host bersama. Manager/callback submission berada di
  `gpu/management/metal_submission.cpp`, di luar source glob hot module.
- `stop()` menolak admission baru, membangunkan producer yang menunggu, dan
  membatalkan queued tickets. Running ticket boleh hidup melampaui manager dan
  tetap menyelesaikan state/counter melalui shared state. Ticket yang ditinggal
  tanpa completion dibatalkan oleh destructor.

## Ownership command

Pipeline, render pass, sampler dan shader caches mengaktifkan policy ownership
pada CacheBase. Texture storage/view memegang shared owners; BufferView yang
sudah memiliki shared owner mendaftarkan owner tersebut. Raw pointer interface
lama tetap tersedia, tetapi seal command juga mengambil leases resource dan
parent graph. Control block resource dibuat oleh kode host registry. View menahan
base texture,
render pass menahan attachment views, dan pipeline menahan shader parents. State
binding renderer juga menahan owner sampai binding diganti/dihapus.

Packet copy mempertahankan leases. Saat encoding, command buffer menahan setiap
owner satu kali. Checkpoint/destructor memindahkan leases ke callback native;
resource wrapper baru dilepas setelah GPU selesai, termasuk pada native error.
Reseal mempertahankan owner packet lama sampai manifest baru selesai dibangun.
Storage texture yang diganti/digabung dipertahankan selama penyalinan dan
leases-nya dipindah ke command buffer sampai completion; jalur lama menghapus
slot storage sebelum memakai pointer lama untuk copy. Regresi pertumbuhan
cubemap menangkap kasus ini. Resource yang masih terikat boleh tetap hidup
setelah completion karena masih diperlukan oleh command berikutnya.

Resource eksternal yang dibuat langsung dan hanya dimiliki unique_ptr/raw pointer
masih kontrak caller: harus hidup selama encoding. Seal tidak mengambil ownership
secara sepihak dan validasi tetap menolak handle yang stale. AS/runtime guest
memory tetap dipertahankan oleh PfifoDecodeState sampai completion; registry ID
bukan pengganti ownership allocation.

Producer kini mengambil snapshot slot texture/sampler renderer ketika descriptor
pool tidak tersedia. Packet tersebut mereset binding dan menerapkan snapshot
secara eksplisit. Resource warisan masuk manifest dan leases; packet tidak lagi
bergantung pada mutation binding renderer di antara prepare dan execute.

Shared ownership membuat GMmu melalui TrackedResource menjadi polymorphic.
Downcast CRTP GenericMmu memakai static_cast agar offset base disesuaikan dengan
benar; reinterpret_cast lama tidak aman bila base bukan di offset nol.

## Cakupan CPU dan pekerjaan lanjutan

CPU dalam perubahan ini adalah pekerjaan host decode/dispatch PFIFO. Guest ARM
threads tetap dikelola Kernel/Process/IThread yang sudah menangani pause, resume,
wait dan stop/join. Perubahan ini **tidak** mengganti scheduler guest, hypervisor
atau Dynarmic, dan tidak mengklaim instruction scheduling CPU baru.

Draw masih di-encode pada worker PFIFO setelah prepare. Belum ada antrean Draw IR
terpisah yang boleh reorder, membekukan guest content, alias dependency graph
baru, migrasi semua non-draw command, atau capture/replay. Dependency API manager
sudah diuji, tetapi dependency guest produksi tetap fence/semaphore dan jalur
coherence yang ada. Perlu content versions dan kontrak hazard sebelum memperluas
ke deferred/reordering execution.

## Validasi

Test baru:

- `hydra-work-manager-tests`: count/byte budgets, dependency success/failure,
  first-terminal-wins, cancellation, stop/wakeup, ticket melewati teardown,
  dan 8.000 admission/completion dari delapan thread.
  Suite ini juga lulus ASan/UBSan. Negative control pada salinan source yang
  mengabaikan sisa budget byte gagal pada assertion budget sebagaimana diharapkan.
- GPU engine: cache/packet teardown dengan leases, reseal, snapshot inherited
  binding, retirement pada GPU completion; checkpoint dan async commit keduanya
  masuk admission manager.
- Shutdown `--check-work-management`: queue saturation, admission cancellation,
  budget recovery, penolakan tanpa lubang fence, CPU→GPU retirement, serta
  cancellation setelah CPU decode selesai ketika GPU ditahan shared event.

Gunakan binary shutdown bundle yang baru, bukan binary plain lama:
`build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests`.

Tidak ada hasil gameplay/FPS atau verifikasi hardware NVIDIA dari perubahan ini.

### Hasil final

Build aplikasi `hydra`, hot module, fixture ABI, dan seluruh target tes terkait
lulus dengan `ninja -j 6`. Dua puluh suite/invokasi final lulus dengan
`MTL_DEBUG_LAYER=1`:

| Pemeriksaan | Hasil |
|---|---|
| WorkManager + resource registry | PASS |
| GPU engine | 45 grup PASS |
| Texture cache (termasuk pertumbuhan cubemap) | 40 grup PASS |
| Shader library | 59 grup PASS |
| Raster state + render hash | PASS |
| Work admission + cancellation saat native GPU tertahan | PASS |
| Scheduling channel + faults + PFIFO decode | PASS |
| Process cancellation + semaphore shutdown | PASS |
| AS binding/lifecycle/bounds + heap resize + GPFIFO input | PASS |
| Shared-memory lifetime | PASS |
| Production hot module, live pixels, ABI rejection, rollback | PASS |
| WorkManager ASan/UBSan + 8.000 concurrent jobs | PASS |
| Negative byte-budget control | Gagal sesuai harapan; source produksi tidak diubah |
| git diff --check | PASS |

Log hasil ada di `/tmp/hydra-management-final-results.json` dan
`/tmp/hydra-management-final-*.log`; detail build di
`/tmp/hydra-management-build9.log`, dengan final incremental/no-op build di
`/tmp/hydra-management-final-build.log`. ASan/UBSan memakai standalone manager
test, bukan seluruh emulator. Native GPU error tidak sengaja dipicu; jalur
failure diuji melalui manager, fault/token tests dan validasi command.

Karena header/layout berubah, rebuild aplikasi dan module harus bersama;
restart aplikasi sebelum memuat module baru. Belum ada benchmark/gameplay.
