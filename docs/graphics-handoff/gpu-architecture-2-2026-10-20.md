# Hydra GPU Architecture 2.0 — Command IR draw, 20 Oktober 2026

Lanjutan terbaru: [pengelolaan eksekusi CPU/GPU, leases dan retirement](execution-management-2026-10-20.md). Batas ownership/binding di catatan awal di bawah telah diperluas oleh tahap tersebut.

Branch: `arvino`. Pengguna mengizinkan implementasi bertahap pada branch fork
ini, lalu meminta melanjutkan pemisahan command/resource. Tidak commit/stage dan
tidak menjalankan game atau GUI.

**Lanjutan:** [Resource Registry dan generation tracking](resource-registry-2026-10-20.md)
menambah sealing/validasi lifetime dan dependency mapping sebelum encoding.
Kontrak serta hasil terbaru ada pada catatan tersebut.

## Implementasi saat ini

```text
ThreeD (register + memory guest)
  -> prepareDraw: pipeline, target/coherence, raster, resource bindings
  -> decode/normalisasi index dan topology, isi parameter draw
  -> command_ir::DrawCommand + seal resource manifest
  -> validate lifetime/mapping
  -> executeDraw(IRenderer, ICommandBuffer, command)
  -> Metal encoding dan sinkronisasi storage write yang sudah ada
```

`DrawCommand` membawa referensi pipeline/render pass, snapshot `DrawState`,
vertex buffer views, uniform/storage/texture/sampler bindings per stage,
parameter draw/index/instance, enable occlusion, dan rentang storage write.
`configureShaderStage` kini merekam binding, bukan mengikatnya langsung pada
renderer. Shader/pipeline lookup, texture alias reconciliation, pengambilan
snapshot uniform dan pembentukan storage view tetap dilakukan di frontend.

`executeDraw` menerapkan binding, memilih draw biasa/indexed, kemudian menunggu
storage writes dan menginvalidasi alias lewat jalur `synchronizeStorageWrites`
yang sudah ada. Consumer tidak membaca register, TIC/TSC, atau GMMU untuk
membentuk draw. Storage ranges menjadi milik command; tidak ada penampung
storage writes tersembunyi di engine 3D.

Kedua entry point engine memakai jalur ini: vertex array, vertex elements,
dan inline index (melalui vertex elements). Topology conversion, primitive
restart dan pembatasan rentang index masih diselesaikan `IndexCache` sebelum
command dieksekusi. Untuk indexed draw, `first` merujuk decoded view; base vertex
array yang dikonversi menjadi indeks tetap dipertahankan.

Visual diagnostics before/after, label GPU capture dan trace tetap di engine.
Pemilihan visual resource dilakukan sebelum snapshot raster agar override
no-depth/no-cull tetap berlaku. Resource preparation tetap berada dalam lock
cache yang sama; consumer dijalankan segera setelah index decode, tanpa queue
baru, reorder atau penundaan ke channel lain.

## Kontrak dan batas lifetime

Ini **IR draw awal yang sudah di-resolve ke resource host**, bukan format
capture portabel atau keseluruhan Architecture 2.0.

- `BufferView` menyalin shared owner yang tersedia, termasuk upload snapshot dan
  decoded index. View yang sejak awal borrowed tetap borrowed.
- Pipeline, render pass, texture view dan sampler masih berupa pointer borrowed
  ke objek/cache yang ada. Consumer harus selesai encoding sebelum objek/cache
  atau mapping terkait dapat berubah/dihancurkan. Command tidak boleh diantrekan
  lintas submit/channel atau disimpan untuk replay kemudian.
- Isi storage/guest-memory views tetap live. Menyalin command tidak membekukan
  seluruh isi memory. Tidak ada resource ID, version, atau alias graph baru.
- `DrawState` masih memakai tipe renderer yang ada, termasuk viewport/scissor
  dan stencil operation Metal. Tidak backend neutral dan tidak boleh
  diserialisasi dengan dump struct mentah.
- Stage aktif selalu membersihkan uniform/storage bindings sebelum penerapan.
  Texture bindings mengikuti perilaku frontend sebelumnya: hanya reset jika
  kedua descriptor pool tersedia. `reset_textures` merekam kebijakan itu secara
  eksplisit; command dengan flag false masih bergantung pada urutan stream.
  Ini belum snapshot mandiri yang bisa diacak/replay secara terisolasi.
- Count atau instance count nol tidak menghasilkan binding, draw, atau
  sinkronisasi storage di consumer. Command nonkosong mengharuskan pipeline,
  render pass, command buffer, dan buffer index untuk indexed draw.
- Draw command masih default instance count 1 pada producer, sesuai jalur lama;
  ini tidak menambah dukungan instancing guest yang sebelumnya belum ada.

Clear, Copy, Dispatch, Query, Barrier, Semaphore dan Present belum dimigrasikan.
Belum ada serializer, capture/replay, jalur lama lewat runtime feature flag,
render-pass optimizer, atau optimasi dirty state baru. Async submission,
scheduler dan completion/lifetime mekanisme lama tetap digunakan.

## Profiling

Aktifkan `HYDRA_PERF_STATS=1`.

`DRAW PREP` (ms/frame):

- `total`: agregat `prepareDraw` dan sealing manifest, termasuk draw yang ditolak, lock wait, tracked-page
  flush dan diagnostic setup. Tidak termasuk index decode atau consumer.
- `pipeline`: lookup/compile shader dan pipeline.
- `targets/coherence`: reconciliation storage/texture dan lookup/memo target.
- `raster snapshot`: normalisasi dynamic raster state.
- `vertex/stage resources`: resolusi dan recording binding serta update memo.

Kolom rinci adalah subset prepare. Sisa total mencakup pekerjaan lain.

`DRAW COMMAND` mengukur consumer (penerapan state/binding, backend draw,
sinkronisasi storage write). `raster apply` adalah subset command, bukan subset
prepare lagi. `DRAW ... binding` lama mengukur native Metal binding di backend,
yang juga tercakup dalam command. PFIFO busy mencakup kedua fase. Jangan
menjumlahkan laporan yang saling mencakup ini sebagai frame time. GPU completion
masih diukur terpisah dan bisa overlap dengan CPU.

Timer baru hanya mengambil clock dan mengubah counter saat profiling aktif.
Belum memisahkan seluruh decode/copy/dispatch/submit/idle. Tidak ada baseline
FPS baru, pengukuran peningkatan performa, atau validasi gameplay sesi ini.

## Validasi

`hydra-gpu-engine-tests` menambah cakupan berikut:

- Raster snapshot vs setter lama: expected RGBA8 pixels, depth rejection,
  scissor, viewport, copy sebelum mutasi dan render/blit/render.
- Consumer command vs jalur manual: direct/U16/U32 index, nonzero vertex/index
  view offsets, first vertex/index, base vertex, base instance dan dua instances.
  Uniform, vertex, storage, texture dan sampler dipakai shader MSL di GPU Metal.
  Buffer caller dilepas setelah command disalin; command harus tetap menjaga
  owner dan menghasilkan pola red/green yang sama byte demi byte.
- Atomic storage counter harus bernilai 16 setelah consumer kembali;
  checkpoint dan invalidasi epoch membuktikan post-draw storage synchronization
  tetap dijalankan, termasuk jika ada side effects GPU.
- Producer stage binding diuji lewat friend accessor: declared guest-to-host
  uniform binding direkam; command lama mempertahankan snapshot upload sementara
  command berikutnya membaca mapping GMMU yang baru. Kebijakan tanpa texture pool
  diperiksa. Tidak mengklaim kelengkapan decoder descriptor/semantik guest.
- Runner viewport mengambil output lewat `prepareDrawState`: semua 16 entry
  dibandingkan helper langsung, viewport/scissor 0 dibanding model Python pada
  500 konfigurasi. Model independen tetap hanya meliputi viewport 0.

Build:

```sh
ninja -C build/swiftui-release -j 6 hydra hydra-hot \
  hydra-gpu-engine-tests hydra-raster-state-tests hydra-render-hash-tests \
  hydra-texture-cache-tests hydra-shader-library-tests \
  hydra-depth-stencil-reference-tests hydra-viewport-reference-tests
```

Lima suite mandiri (gpu-engine, raster-state, render-hash, texture-cache,
shader-library) dijalankan dengan `MTL_DEBUG_LAYER=1`. Dua runner referensi
memerlukan file kasus dari wrapper Python:

```sh
python3 tests/depth_stencil_reference.py \
  build/swiftui-release/bin/hydra-depth-stencil-reference-tests \
  --report /tmp/hydra-command-ir-depth-stencil.json
python3 tests/viewport_reference.py \
  build/swiftui-release/bin/hydra-viewport-reference-tests
MTL_DEBUG_LAYER=1 HYDRA_DISABLE_RASTER_CACHE=1 \
  build/swiftui-release/bin/hydra-raster-state-tests
```

Hasil akhir pada mesin ini, seluruh tes GPU dengan Metal API validation:

| Pemeriksaan | Hasil |
|---|---|
| Build aplikasi, production hot module, target tes/fixture | PASS |
| GPU engine, snapshot dan command/resource tests | PASS (41 baris PASS) |
| Raster cache aktif / nonaktif | PASS; 24 updates/16 skips dan 40 updates/0 skips |
| Render hash | PASS |
| Texture cache | PASS (40 baris PASS) |
| Shader library | PASS (59 baris PASS) |
| Python depth/stencil → Metal | 6656/6656 cocok |
| Python viewport/scissor melalui snapshot | 500 konfigurasi, nol mismatch |
| Hot reload | PASS; production module, pixel output, pinning, ABI rejection, rollback |
| `git diff --check` | PASS |

Sensitivity control: consumer sementara dipaksa memakai satu instance untuk
non-indexed draw. Tes berhenti dengan exit 1 pada expected count 16 (pesan
`Draw command runs exactly two instances and publishes storage writes`). Kode
asli dipulihkan, semua binary dibangun ulang, seluruh regresi di atas lulus.
Aplikasi dan GUI tidak dijalankan untuk tes ini.

Wrapper referensi dijalankan offline dengan model yang sudah ada, tanpa audit
ulang header NVIDIA. Ini bukan NVIDIA hardware trace atau validasi gameplay.

Log: `/tmp/hydra-command-ir-build-final.log`,
`/tmp/hydra-command-ir-final-*.log`,
`/tmp/hydra-command-ir-control-test.log`.
Build memiliki warning deployment target dari dependensi fmt/SDL lokal, tanpa
error kompilasi/link akhir. ABI engine berubah: jalankan build aplikasi baru
dengan restart sekali; hot reload dari proses lama tidak dapat memigrasikan
layout tersebut. Tes hot reload sengaja memuat fixture ABI salah dan memastikan
modul ditolak; pesan rejection itu bukan kegagalan tes.

## Pekerjaan berikutnya (OPEN)

1. Resource ID/generation dan pemeriksaan mapping sudah ditambahkan pada
   lanjutan registry. Berikutnya: version isi resource, rentang guest memory,
   pin ownership untuk pointer borrowed, write CPU/GPU dan texture aliasing.
2. Capture rentang command kecil: command guest, shader mentah, resource content
   yang konsisten pada titik konsumsi dan output pembanding. Capture IR sendiri
   tidak cukup; kebijakan inherited binding juga harus diekspansi menjadi state.
3. Migrasikan operasi non-draw dengan urutan efek sama. Tambahkan pemilihan jalur
   lama/baru dan differential validation dengan memory/resource terisolasi.
4. Tambahkan replay, lalu optimasi berdasarkan profil. Pengguna menjalankan game
   untuk baseline scene/resolusi/cache warm yang sama, sesuai aturan kerja repo.

Jalur instruksi dan backlog kompatibilitas sebelumnya tetap **OPEN**. Build dan
regresi terarah tidak menyatakan seluruh GPU/Architecture 2.0 selesai.
