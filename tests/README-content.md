# Game updates and DLC

The library's **Install Update / DLC** action copies an NSP/XCI to a unique
staging file, validates its CNMT and referenced NCA SHA-256 hashes, keys, ranges
and supported BKTR tables, then renames it into `content/packages/<SHA256>.nsp`
(or `.xci`) under Hydra's app data directory. Failures remove staging files;
duplicate imports preserve the existing package. No base game or save is changed.
The CNMT footer is treated as an opaque identifier: production metadata may
retain its development-build digest, so hashing production CNMT bytes against
that footer would incorrectly reject retail packages. Referenced NCA hashes are
still verified.
The importer runs off the SwiftUI main thread and copies its result text before
returning from the native worker thread. It is disabled during emulation.

At load, CNMT application IDs and versions select the newest matching patch and
DLC. A Program NCA is matched through content ID/type/index rather than file
size/order. The updated executable and RomFS are validated before the loader
uses them. The selected update's Control NCA supplies display metadata when
present. BKTR relocation ranges read from either the base section or the patch's
physical section; patch physical reads split across AES-CTREx generation
boundaries, while the two tables use ordinary CTR. No full RomFS extraction is
needed. A virtual NCA relocates sections to accommodate a larger patched RomFS.
Backing storage survives in open streams, and a stream opens each backing file
once, independently of the number of relocation entries.

DLC is loaded once per game boot into an OS-owned map, filtered to the game's
application ID and required update version. `aoc:u` commands 2/3/5/7 enumerate
indices, return the conventional application+0x1000 base ID, and prepare existing
DLC. Command 3 respects pagination and output-buffer capacity. FSP command 202
opens the installed DLC's RomFS and rejects missing or foreign application IDs.
The list-changed event remains unsignalled because installation during gameplay
is disabled; restart the game after importing content.

## Verification

```sh
python3 tests/generate_content_fixtures.py build/content-fixtures
cmake --build build/swiftui-release --target hydra-content-tests hydra-shutdown-tests -j 4
./build/swiftui-release/bin/hydra-content-tests build/content-fixtures
./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-installed-dlc
```

Both targets use existing test options (`HYDRA_XCI_TESTS` and
`HYDRA_METAL_CACHE_TESTS`). The service test requires the same configured
firmware and app resources as the existing shutdown tests.

The Python generator creates only synthetic data and independent cryptography
fixtures. Native tests compare CTREx plaintext, merged RomFS bytes, 1,000 random
cross-boundary reads, retained backing lifetime, and multi-bucket trees. They
exercise installed and bundled patches, full replacements, newest-version
selection independent of install order, duplicate imports, DLC data/version
requirements, and failed installations for bad hashes/CNMT, missing content,
invalid trees and out-of-range patch relocations. Service tests use the real IPC
argument decoder for count/list/base/prepare and open a real DLC IStorage through
FSP, checking its returned size and rejecting foreign/missing data IDs.

## Limits

- NCA3 only. BKTR supports one root index and up to 2,046 leaf buckets, each
  0x4000 bytes, with metadata limited to 64 MiB per table. Deeper trees, sparse
  sections, compressed NCA sections, and newer encryption variants are rejected.
- Install uncompressed NSP/XCI packages. NCZ remains available for base-game
  loading, but compressed update/DLC installation is explicitly rejected.
- Common tickets require matching `titlekek_XX`; personalized tickets are not
  supported. Hashes detect corruption; package signatures are not authenticated.
- Main Program/Data index 0, conventional DLC IDs, and legacy/ordinary add-on
  content metadata are supported. Multi-program updates, DLC data-patch
  dependencies, custom NACP add-on base IDs, live installation and eShop purchase
  services are outside this implementation.
- Container-based base games use the store. Extracted folders and standalone
  NCA/NX paths do not. Packages are kept whole; importing a bundled XCI also
  copies its base content. Space is needed for that copy and older versions.
- No title-specific retail gameplay validation is implied by synthetic tests.

Format/ABI references:
[SciresM/hactool BKTR structures](https://github.com/SciresM/hactool/blob/master/bktr.h),
[hactool CTREx and relocation reads](https://github.com/SciresM/hactool/blob/master/nca.c),
[Atmosphère CNMT structures](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_meta.hpp),
[CNMT footer format](https://switchbrew.org/wiki/CNMT#Digest),
[ReSwitched IPC definitions](https://reswitched.github.io/SwIPC/ifaces.html#nn%3A%3Aaocsrv%3A%3Adetail%3A%3AIAddOnContentManager).
