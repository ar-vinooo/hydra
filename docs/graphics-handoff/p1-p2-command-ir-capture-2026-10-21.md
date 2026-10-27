# P1 dan P2: Command IR, capture/replay, versi isi, saturasi (21 Oktober 2026)

Semua bukti berasal dari tes headless (`MTL_DEBUG_LAYER=1`); tidak ada yang diuji di game.

## Korelasi draw (P1)

`FrameTracer` menulis, per draw, fitur shader yang dilewati (`VS/FS unsupported: ...`) dan satu baris per tekstur yang
di-sample (`tex<binding> (host n): tic= tsc= addr= fmt= WxH levels= layout= type=`). `VisualDiagnostics` mencatat
event `draw_unsupported` dan `texture_bindings`. Satu pasang alat untuk menjawab "draw ini memakai apa".

## Command IR untuk Clear dan Copy (P1)

`command_ir/resource_set.*` (identitas resource, lease kepemilikan, mapping GPU, manifest isi), `clear.*`
(`ClearCommand`) dan `copy.*` (`CopyCommand`, blit tekstur 2D). `ThreeD::clearBuffer` dan `TwoD::copy` kini membangun
command, menyegelnya (`sealClear`/`sealCopy`), lalu mengeksekusinya. Sama seperti draw: sumber daya yang sudah dilepas
atau mapping yang berubah menolak command dan memfault channel (kode 25). Pass kosong dianggap tidak ada yang dihapus.
Urutan eksekusi tetap sinkron; belum ada penundaan. Tes: `testClearAndCopyCommands` (+ semua tes clear dan blit lama).
Belum di IR: compute dispatch, barrier, query, present.

## Versi isi resource (P2)

`resource/content_version.*`: `ContentVersions` memberi cap versi pada rentang alamat host setiap ada tulisan (CPU/DMA lewat
`IRenderer::invalidateMemory`, GPU lewat eksekusi draw/clear/copy); `latest(range) > V` berarti byte yang dibaca command
berubah sejak disegel. Dibatasi 16.384 rentang; melampauinya menaikkan lantai sehingga hanya melaporkan perubahan
berlebih, tidak pernah kurang. `ContentManifest` memuat bacaan (dengan versi) dan tulisan tiap command;
`dependsOn()` menyatakan apakah dua command harus tetap berurutan (RAW/WAR/WAW atas rentang yang tumpang tindih, jadi
juga alias). `BufferView` kini membawa `guest_range`. `DrawCommand`, `ClearCommand` dan `CopyCommand` mengisi manifest
saat disegel; `validateDrawContents` memeriksanya. Prasyarat untuk eksekusi tertunda: command yang disusun lalu
dieksekusi belakangan memanggil `validateDrawContents`, atau mengambil snapshot. Granularitas per rentang alamat, bukan
per byte atau subresource. Tes: `resource_registry_tests` (versi, tumpang tindih, dependensi, penggusuran, 4 thread) dan
`testDrawContentVersions`.

## Capture dan replay (P1)

Level host: yang direkam adalah command yang sudah di-resolve engine, bukan stream guest.
- `capture/draw_capture.*`: `Recorder` (merekam draw/clear: shader MSL hasil terjemahan, pipeline, pass, state, byte buffer
  tiap versi, piksel awal tekstur saat pertama terlihat), `CaptureFile` (format biner `HYDRACAP` v1, terikat pada build:
  ukuran struct state dicek), `replay()` (membangun ulang objek di renderer baru, menjalankan command berurutan, membaca
  kembali tekstur yang ditulis dan membandingkannya), `LiveCapture` (hidup bila `HYDRA_GPU_CAPTURE=<dir>`; file bernama
  `trigger` di dir itu, isi opsional `commands=N`, default 2, memulai capture pada frame berikutnya; hasil
  `capture_<n>.hcap`). Hook di `ThreeD::executeDrawCommand` dan `clearBuffer` (rekam sebelum eksekusi, `commandExecuted` sesudahnya: hasil akhir dibaca setelah command terakhir dieksekusi, bug yang ditangkap `testCaptureAndReplay`) dan `System` (per present).
- Renderer: `readTextureContents`, `writeTextureContents`, `readBufferContents` (blocking, Metal di `metal/texture_io.cpp`;
  warna, depth16/32, stencil, D32S8 dua plane, terkompresi per blok; tekstur buffer tidak didukung).
- Alat: `build/swiftui-release/bin/hydra-gpu-replay <file.hcap> [--dump <dir>]` (exit 0 bila piksel sama).
- Tes `testCaptureAndReplay`: clear + dua draw (buffer yang sama berisi byte berbeda) direkam, diserialisasi,
  dimuat, dan diputar di `Gpu` baru: piksel identik. Kontrol: isi buffer diubah, urutan ditukar, objek hilang, file
  terpotong atau bermagic salah semuanya ditolak/berbeda. Jalur live (mulai, rekam, tulis file, muat, putar) juga diuji lewat `LiveCapture::start`. File juga diputar oleh `hydra-gpu-replay` di proses terpisah
  (`HYDRA_TEST_CAPTURE_DIR` menyimpan file tes).

Batas yang jujur: tidak memuat stream guest, GMMU, atau shader Maxwell (hanya hasil terjemahan MSL-nya), jadi bug di
decoder/engine tidak bisa direproduksi, hanya bug sisi host (state Metal, isi tekstur/buffer, MSL). Tekstur terkompresi
tidak diuji; storage buffer yang ditulis shader tidak ikut sebagai output; capture dibatasi 1 GiB. Buffer Shared yang
ditulis CPU di antara draw yang belum selesai menghasilkan data berbeda dari engine (engine memakai snapshot immutable),
dan Recorder membaca buffer saat draw direkam. Belum dicoba pada game.

## Saturasi submission Metal (P2)

`testSubmissionSaturation`: 64 command buffer yang menunggu `MTLSharedEvent` memenuhi seluruh slot admission; submitter ke-65
menunggu (tidak melampaui budget); begitu event dilepas semuanya selesai dan yang menunggu masuk. Lalu 8 thread x 150
submission dan burst di belakang pekerjaan tertahan: semua sukses, `peak_pending <= 64`, tidak ada kebuntuan. Slot dibebaskan
hanya oleh completion GPU, jadi thread yang menunggu admission tidak menahan pembebasannya. Tidak ditemukan bug.

## Baseline performa PFIFO/Metal (P2)

`HYDRA_GPU_BENCH=1 hydra-gpu-engine-tests` mencetak (M1, release): tulis versi isi ~33 ns, baca ~44 ns; seal draw ~0,8 us
(0,5 us tanpa buffer; ~75 ns per buffer: tracking registry dan manifest), cek isi ~36 ns, encode draw ~2 us, submission
kosong ~2,7 us. Dua optimasi dari pengukuran ini: satu versi untuk semua bacaan satu command (tanpa kunci per bacaan) dan
tulis ulang rentang yang sama hanya meng-cap ulang (80 -> 33 ns). Angka per frame game (`DRAW PREP`, `DRAW COMMAND`,
PFIFO CPU, GPU) tetap harus diambil dengan `HYDRA_PERF_STATS=1` pada game; biaya seal sekitar 0,8 us x jumlah draw
per frame adalah angka yang dibandingkan. Tidak ada optimasi lain yang diklaim.

## Regresi

`run_python_references.sh` ALL PASS; shader-library 60, texture-cache 41, gpu-engine 49, raster-state, render-hash,
register-write, frame-trace, visual-diagnostic, resource-registry, dma-slice, work-manager lulus. `hydra`, hot module dan
`hydra-gpu-replay` ter-build. Header banyak berubah: restart aplikasi. Tidak di-stage atau di-commit.

## Uji live di Crash 4 (21 Oktober 2026, pagi)

Sesi pertama: capture live ditolak karena command pertama memakai tekstur buffer (`rg32float`, 1D buffer). Perbaikan:
tekstur buffer direkam lewat buffer penyangganya (`readTextureContents`/`writeTextureContents`), dan tekstur yang tidak
bisa dibaca kini dicatat sebagai `notes` di file (format versi 2) alih-alih membatalkan capture.

Sesi kedua (build baru, adegan pantai): `commands=6` menghasilkan `capture_0.hcap` (6 MB: 6 draw dengan pipeline yang
sama, indexed 48 indeks, 9 tekstur per draw, 8 tekstur, 10 buffer, 2 shader MSL tanpa fitur unsupported). `hydra-gpu-replay`
di proses terpisah: `MATCH`, piksel identik. Artinya jalur live (hook engine, pembacaan GPU, penulisan file, replay) bekerja
pada game sungguhan, bukan hanya di tes. Artefak putih/noise yang terlihat di sesi pertama tidak muncul di sesi kedua
(nondeterministik, atau terkait keadaan sesi), jadi capture ini bukan capture dari draw yang rusak.

Baseline performa dari game (`--perf-stats`): ~8 fps; pfifo (CPU encode) ~123 ms/frame untuk ~4385 draw (3,9 daftar), present
wait ~123 ms, Metal GPU ~41 ms/frame, ~9,5 command buffer/frame. Biaya seal+encode yang diukur di benchmark (~3 us per draw)
hanya ~13 ms dari ~123 ms; sekitar 25 us per draw berada di persiapan draw di engine (pipeline, texture/buffer cache,
state), bukan di command IR. Di situlah optimasi PFIFO harus dicari.

## Capture satu frame di Crash 4 dan temuan artefak peti (21 Oktober 2026)

`commands=6000` pada adegan hutan menghasilkan `capture_1.hcap` (283 MB, 6.000 command, 665 tekstur, 166 shader) tanpa
menekan memori game. Batas ukuran kini berakhir rapi: `limit_mb=N` di file trigger (default 1024) menghentikan capture dan
menyimpan command yang sudah terekam, dengan catatan di file; `skip=N` melewati N command pertama.

Alat replay (`hydra-gpu-replay`): `--png <dir> --png-every K` (PNG per K command, format HDR di-tone-map, index.txt),
`--commands N`, `--from N`, `--watch tex,x0,y0,x1,y1` (command yang mengubah suatu region), `--inspect N` (draw, shader,
tekstur yang di-sample dan PNG-nya, BC1-BC7 lewat bcdec di /private/tmp/hydra-bcdec).

Temuan: replay di luar game mereproduksi artefak yang terlihat di game (noise hijau-gelap pada peti dan tekstur Crash di
target `rg11b10float` 1280x720, tekstur 241). Draw yang menggambar peti: command 1555 (pipeline 49, pass 6, 2.976 indeks).
Tekstur 2D yang di-sample (albedo BC1 sRGB, BC5, mask r8, BC1) terlihat benar di PNG, dan dua tekstur buffer vertex
(`rgba8snorm` 2864, `rg16float` 4296) berisi nilai masuk akal (UV 0,005-0,99, tanpa NaN), jadi penyebabnya belum
ditemukan. Kandidat berikutnya: mapping slot tekstur/sampler pada draw itu, enam tekstur `1280x25` (tipe 2D tanpa mip,
tinggi 25) yang ikut terikat, atau terjemahan fragment shader 126 KB. Replay tidak deterministik penuh: tekstur 1280x720
tertentu berbeda dari run asli antar-replay karena capture hanya memuat draw dan clear, bukan copy/compute/DMA di antaranya.

## Perbaikan noise peti di Crash 4: RT yang sudah di-flush lalu ditimpa guest (21 Oktober 2026)

Dengan capture frame penuh dan `hydra-gpu-replay` (`--watch`, `--inspect`, PNG per langkah) artefak peti dipersempit ke
command 1555 (pipeline 49) dan peta normal BC5 256x256 yang mip 0-nya punya kolom blok rusak. `reload_sampled` /
`reload_always` (muat ulang dari guest) tidak memperbaikinya, jadi datanya rusak di memori guest. Log
`texture_events.log` menunjukkan: RT 2x2 `bgra8unorm_srgb` lama di tengah mip 0, sudah di-flush, ditimpa guest (data streaming),
lalu saat BC5 dibuat ia dipakai sebagai sumber alias dan ditulis balik ke memori guest.

Akar: `TextureCache::invalidateMemoryImpl` hanya memanggil `markStale` bila RT punya data GPU yang belum di-flush; RT yang
sudah di-flush hanya mendapat `cpu_modified_timestamp`, sehingga pemindaian alias menganggapnya sumber "lebih baru dan
masih segar". Perbaikan: storage yang pernah ditulis GPU juga ditandai stale pada bagian yang ditimpa guest. Tes:
`hydra-texture-cache-tests` (RT yang di-flush lalu ditimpa guest, di atasnya BC5 dan alias seukuran); gagal sebelum
perbaikan. Di game: peta normal mip 0 dan peti kayu bersih setelah perbaikan; sesi berikutnya (tanpa flag) juga bersih
saat maju-mundur. Penyebab sisa noise (mip >= 2 albedo BC1 di satu sesi, bintik di kepala Crash) belum terpecahkan.

Dugaan shader cache disingkirkan: replay capture dengan `HYDRA_DISABLE_SHADER_CACHE=1 HYDRA_DISABLE_RASTER_CACHE=1`
menghasilkan PNG identik byte per byte. Flag diagnostik `no_block_alias` (hot, file di trace dir) menonaktifkan salinan
GPU langsung blok<->texel; dengan flag menyala noise sisa tidak berubah. Capture v3 memuat byte guest tiap tekstur;
`--inspect` membandingkannya dengan isi host per mip.
