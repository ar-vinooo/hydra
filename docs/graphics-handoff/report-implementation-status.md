# Status implementasi laporan correctness GPU

Catatan ini memetakan rekomendasi laporan `deep-research-report (1).md` ke source
lokal. Laporan berisi invariant dan risiko yang sebagian belum dikonfirmasi;
contoh pseudocode di dalamnya bukan bukti bahwa fungsi Hydra tertentu rusak.

## Pemetaan source

| Domain | Source / fungsi | Perilaku yang ditangani |
| --- | --- | --- |
| Identitas channel | `gpu/pfifo.hpp`, `PfifoDecodeState`; `gpu/gpu.hpp`, `GpuChannelContext` | Sequence, packet, macro, engine dan puller state per channel |
| Urutan method | `gpu/pfifo.cpp`, `processMethodArg`, `dispatchMethod` | Sequence monotonik dan sumber word, termasuk argumen macro yang tertunda |
| Block/resume | `gpu/pfifo.cpp`, `ready`, `threadFunc` | Acquire menahan channel sendiri; channel producer lain tetap bisa berjalan |
| Semaphore | `gpu/gpu.cpp`, `pullerMethod`; `gpu/engines/3d.cpp`, `setReportSemaphore`; `gpu/engines/copy.cpp`, `releaseSemaphore` | Predicate acquire, checkpoint producer dan publikasi release/report |
| Mapping / lifetime AS | `gpu/gmmu.cpp`; `gpu/resource/mapping_capture.*` | Dependency mapping, generation dan retained address space |
| Versi backing fisik | `gpu/resource/content_version.*`, `ContentVersions` | Rentang overlap, epoch CPU/GPU, status pending/completed/failed; completion lama tidak mengambil alih write baru |
| Alias texture/buffer | `gpu/renderer/texture_cache.*`, `core/hot/texture_cache_hot.cpp`; `gpu/renderer/buffer_cache.*` | Invalidation/flush berdasarkan rentang backing yang overlap, termasuk writeback parsial |
| Resource IR | `gpu/command_ir/{draw,clear,copy,compute,resource_set}.*` | Sealing, reads/writes, identitas resource, owner, mapping dan penolakan binding yang berubah |
| Compute backend | `gpu/renderer/metal/renderer.*`, `core/hot/routes.cpp` | Status encode, batas dispatch/binding, profiling kernel sekali, kegagalan tidak mengeluarkan write epoch sukses |
| Completion / retirement | `gpu/renderer/command_buffer.hpp`; `gpu/management/metal_submission.cpp`; `gpu/renderer/metal/completion_queue.*` | Resource ditahan hingga submission selesai; callback GPU dan publikasi report memiliki tahap terpisah |
| Snapshot command capture | `gpu/capture/draw_capture.*`, `tests/gpu_replay_tool.cpp` | Format 4 draw/clear/copy/compute, origin, alias buffer, pembandingan output storage/texture dan pembacaan format 3 |
| Guest event capture | `gpu/capture/guest_events.*` dan hook engine/PFIFO/renderer | Word/method, lifecycle channel/AS, snapshot/input/writeback, DMA, synchronization, epoch, report, native submission dan completion |
| Semantic replay | `tools/replay_guest_events.py` | Rekonstruksi memory/ownership/dependency; input yang hilang tetap unknown; footer/pending/failure eksplisit |
| Native guest replay | `tests/guest_replay_tool.cpp`, `Pfifo::replayWord` | Word rekaman melewati decoder dan engine produksi, method diverifikasi, output native dibandingkan |
| Trace / differential | `tools/check_gpu_trace.py`, `tools/compare_gpu_replays.py` | Urutan/block/resume dan perbandingan output byte-hash dengan reference eksplisit |
| Stress / regression source | `tests/guest_event_tests.py`, `tests/shutdown_tests.cpp`, `tests/gpu_engine_tests.cpp`, `tests/content_version_tests.cpp` | Kegagalan format/order, ownership, producer/consumer GPU, partial alias, overwrite CPU, compute dan completion |

Path `gpu/...` pada tabel relatif terhadap `src/core/hw/tegra_x1/`;
`core/hot/...` relatif terhadap `src/`.

## Batas penyelesaian

Kelanjutan Windows ini menambah implementasi native replay, identitas native
submission/completion, pembanding differential dan skenario stress yang
sebelumnya masih disebut sebagai pekerjaan source tersisa. Cara pemakaian dan
batas replay ada di [guest-event-stream.md](guest-event-stream.md).

Build dan seluruh eksekusi validasi kelanjutan ini sengaja tidak dijalankan,
sesuai instruksi pengguna. Keberadaan source dan skenario uji tidak berarti
uji lulus, kompatibilitas Maxwell menyeluruh, atau bebas error. Hasil historis
di dokumen lain tidak mencakup penambahan ini.

Perangkat guest nyata dan capture reference perangkat tidak tersedia di sesi
ini. Alat pembanding menerima reference yang dikumpulkan terpisah; tidak ada
hasil hardware yang diklaim atau dibuat-buat.

Pengurangan barrier dan batching P2 belum diaktifkan. Laporan secara eksplisit
mensyaratkan test P0 hijau sebelum optimasi tersebut. Karena pengguna menunda
pengujian, prasyarat itu belum dibuktikan. Jalur konservatif dipertahankan;
penundaan ini tidak disamarkan sebagai optimasi yang sudah selesai.
