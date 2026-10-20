# Resource Registry dan generation tracking — 20 Oktober 2026

Lanjutan terbaru: [pengelolaan eksekusi CPU/GPU, leases dan retirement](execution-management-2026-10-20.md). Batas ownership/binding di catatan awal di bawah telah diperluas oleh tahap tersebut.

Branch `arvino`, lanjutan Command IR atas instruksi pengguna. Tidak stage/commit,
tidak menjalankan game atau GUI. Status keseluruhan GPU Architecture 2.0 OPEN.

## Modul dan kontraknya

`gpu/resource/resource_lifetime.hpp` memberi token lifetime pada buffer, texture,
texture view, sampler, shader, pipeline, render pass dan GMMU. Token tidak
memiliki objek GPU; penghancuran objek otomatis me-retire registrasinya.

`ResourceRegistry` dimiliki per `IRenderer`, tanpa singleton process-wide:

- `track(lifetime, kind, object)` menghasilkan handle ID/generation. ID monotonik
  dalam registry, tidak dipakai ulang. Tracking objek yang sama mempertahankan ID.
- `ResourceLifetime::invalidate()` menaikkan generation; salinan handle lama
  tidak berlaku. Alamat yang dipakai objek baru mendapatkan ID baru.
- `inspect` / `matches` memeriksa registry pemilik, lifetime, ID, generation,
  jenis dan alamat. Handle menyimpan weak reference; validasi tidak perlu
  men-dereference pointer resource yang sudah mati.
- Metadata dihapus saat objek dihancurkan. Menghancurkan registry membuat handle
  tidak berlaku walaupun objek bertahan. Registrasi satu objek ke dua registry
  hidup sekaligus ditolak, bukan memindahkan identitas secara diam-diam.
- Tracking/invalidate/destruction memerlukan serialisasi pemilik objek yang
  sudah digunakan engine/cache. Inspeksi bukan pengganti lock atau ownership.

Ini generation **identitas/lifetime dan invalidasi eksplisit**, bukan counter
isi resource untuk setiap write CPU/GPU. ID juga belum berarti rentang guest
memory, subresource identity, alias graph, ataupun resource snapshot portabel.

## Integrasi command

Frontend mengambil handle address space sebelum persiapan resource. Setelah
index decode, `sealDrawResources` mengumpulkan referensi langsung dan dependency
objek parent: shader pipeline, attachment view dan base texture, serta base
texture dari binding stage. Manifest memegang handle dan alamat yang diperiksa.

`executeDraw` menolak command nonkosong yang belum disegel, kehilangan input
wajib, berganti pointer tanpa reseal, memiliki resource/generation basi, atau
memiliki mapping terpakai yang berubah. Validasi dilakukan sebelum binding dan
native encoding. Return `false` diteruskan oleh engine sebagai fault channel
kode 25, sehingga penolakan tidak dilaporkan sebagai draw/fence berhasil.
Command count/instance count nol tetap no-op yang tidak membutuhkan resource.

Command harus disegel ketika objek masih hidup, dalam serialisasi pemilik cache
GPU yang sudah ada. Command yang sudah disegel boleh diperiksa setelah objek
retire, tetapi menyegel ulang pointer yang sudah mati tidak diperbolehkan.
Pointer borrowed tidak menjadi owned karena didaftarkan. Guard tidak menahan
objek selama encoding atau sampai GPU selesai; kontrak eksekusi langsung dan
sinkronisasi teardown/PFIFO yang ada tetap diperlukan. Tidak mengklaim menutup
semua race penghancuran dari thread lain setelah titik validasi.

## Mapping GMMU dan cache

`MappingCaptureScope` merekam lookup GMMU pada thread yang menyiapkan draw:
`base GPU, pointer host, ukuran mapping, alamat backing guest`. `findAddr`,
`findAddrImpl`, dan `unmapAddr` tetap memakai resolusi lama, sambil merekam hasil
ke scope aktif. Scope bersarang mengisi scope induk untuk address space yang
sama dan menduplikasi setiap versi mapping paling banyak sekali per scope.

- Generation GMMU yang sama memberi jalur validasi cepat.
- Bila generation berubah, hanya mapping yang tercatat dibandingkan lagi.
  Update mapping yang tidak dipakai tidak langsung menolak draw. Pembandingnya
  kesetaraan translation metadata; remap ke backing dan rentang yang persis
  sama dianggap setara, bukan versi alokasi guest yang baru.
- Remap, unmap, heap rebase, atau perubahan rentang mapping yang dipakai
  membatalkan command lama. Granularitasnya satu mapping, bukan jumlah byte
  minimum yang dibaca; pemecahan mapping yang sama juga ditolak konservatif.
- Lookup validasi memegang shared mapping lock untuk satu pemeriksaan konsisten;
  lookup ini tidak merekam ulang ke vector dependency yang sedang diperiksa.
- GMMU yang dihancurkan atau dibatalkan ditolak sebelum pointer lamanya dibaca.

Memo texture dan render pass sekarang membawa dependency lookup asal ke command
berikutnya. Identitas GMMU dari registry serta generation mapping ikut syarat
reuse memo. Ini menutup celah ketika remap tidak mengubah register/TIC atau
texture-cache epoch. Objek GMMU baru pada alamat yang sama dan counter numerik
yang sama juga tidak boleh menghidupkan memo lama.

Capture scope mencakup persiapan draw, diagnostic before-draw dan index decode;
lookup diagnostic dapat membuat dependency lebih konservatif. Data memory guest
yang ditulis in-place tanpa perubahan mapping tidak dibekukan oleh modul ini.

## Hot reload dan profiling

Consumer `command_ir/*.cpp` sekarang masuk manifest sumber hot reload. Registry,
retirement dan TLS pencatat mapping tetap dimiliki host; tidak ada salinan TLS
per modul yang membuat lookup hilang dari capture. Header/layout berubah, jadi
build aplikasi baru memerlukan restart sekali. Perubahan implementasi registry
atau GMMU host berikutnya juga memerlukan rebuild/restart.

`DRAW PREP total` mencakup `prepareDraw` ditambah sealing manifest, sebagai dua
interval yang tidak bersarang. Rincian pipeline/target/snapshot/resource tetap
subset dari total itu; sisa total mencakup sealing dan pekerjaan lain. Validasi
ulang pada consumer termasuk `DRAW COMMAND`. Tidak ada klaim FPS atau pengukuran
keuntungan performa. Registry/manifest menambah biaya CPU yang perlu diprofilkan.

## Validasi dan reproduksi

```sh
ninja -C build/swiftui-release -j 6 hydra hydra-hot \
  hydra-resource-registry-tests hydra-gpu-engine-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-resource-registry-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-gpu-engine-tests
```

Suite registry menguji repeated tracking, invalidasi generation, penggunaan
ulang alamat, registry asing, retirement, teardown registry, 5.000 resource
sementara tanpa penumpukan metadata, scope lookup bersarang, dependency cache,
validasi yang tidak memutasi capture, 2.000 pemeriksaan dengan thread lain
mengubah mapping yang tidak dipakai, unmap dan heap rebase.

Suite GPU menguji penolakan pipeline/pass/shader/buffer/sampler/base texture
setelah dihancurkan, invalidasi generation, perubahan pointer, mapping yang
terpakai vs tidak terpakai, retirement GMMU, serta memo render target setelah
remap dan penggunaan ulang alamat GMMU dengan generation numerik yang sama.
Penolakan di engine juga diuji menghasilkan channel fault 25 dan notification.
Tes command/pixel, owner buffer dan storage completion sebelumnya tetap dijalankan.

Hasil akhir pada mesin ini:

| Pemeriksaan | Hasil |
|---|---|
| Build aplikasi, hot module, binary tes dan fixture | PASS |
| Resource Registry + mapping dependencies | PASS |
| GPU engine, command/resource validation dan memo remap | PASS (43 baris PASS) |
| Texture cache | PASS (40 baris PASS) |
| Shader library | PASS (59 baris PASS) |
| Raster state, cache aktif/nonaktif | PASS; 24 update/16 skip dan 40 update/0 skip |
| Render hash | PASS |
| Python depth/stencil → Metal | 6656/6656 cocok |
| Python viewport/scissor | 500 konfigurasi, nol mismatch |
| Hot reload | PASS; production module, live pixels, pinning, ABI rejection dan rollback |
| GPU address-space bounds/input | PASS |
| AS_GPU lifecycle | PASS |
| `git diff --check` | PASS |

Tes GPU memakai Metal API validation. Kontrol sensitivitas: pemeriksaan
perbedaan generation pada registry sengaja dimatikan sementara; suite registry
berhenti dengan exit 1 pada `Invalidation advances the generation without
reviving the old handle`. Kode dipulihkan, semua binary dibangun ulang, kemudian
seluruh pemeriksaan akhir di atas lulus.

Dua tes AS_GPU memakai binary bundle terbaru, bukan binary lama yang masih ada
di `bin/hydra-shutdown-tests`:

```sh
MTL_DEBUG_LAYER=1 \
  build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests \
  --check-gpu-address-space
MTL_DEBUG_LAYER=1 \
  build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests \
  --check-gpu-as-lifecycle
```

Percobaan pertama pada path binary lama berhenti saat membaca fixture kosong;
hasilnya tidak dipakai sebagai regresi build baru. Kedua perintah bundle di atas
kemudian lulus. Ini tes headless dengan UI stub, tanpa game/GUI.

Log akhir: `/tmp/hydra-registry-final-build.log`,
`/tmp/hydra-registry-final-*.log`; kontrol di `/tmp/hydra-registry-control.log`.
Build tetap memberi warning deployment target fmt/SDL dari dependensi lokal,
tanpa error kompilasi/link akhir. Wrapper Python memakai model yang sudah ada,
tanpa audit ulang header NVIDIA atau NVIDIA hardware trace pada sesi ini.

## Batas dan lanjutan

- Integrasi manifest ini meliputi draw 3D. Compute/copy/clear/query/semaphore
  belum memakai kontrak command/registry yang sama.
- Kebijakan texture pool kosong masih mengikuti jalur lama (`reset_textures`
  false). Binding yang diwarisi dari state renderer belum diekspansi menjadi
  manifest mandiri; ini tetap dependency stream, belum capture/replay terisolasi.
- Belum ada pin ownership untuk pointer borrowed sampai completion, version isi
  resource, dependency graph read/write, alias tracking baru, serializer atau
  replay. Semua ini tetap diperlukan sebelum deferred execution.
- Regresi terarah tidak membuktikan seluruh semantik guest atau gameplay.
