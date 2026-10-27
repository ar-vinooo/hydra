# Game updates and DLC — 22 October 2026

Implemented update/DLC import in the SwiftUI library (**Install Update / DLC**),
backed by a C API and an immutable local package store. Uncompressed NSP/XCI
imports are staged, checked against CNMT content IDs, sizes, types and referenced
NCA SHA-256 hashes, then atomically published. Duplicate imports are idempotent.
The highest matching CNMT version is selected independently of import order.
Bundled update/DLC content is also considered without a separate import.

The loader applies full Program replacements and BKTR base/patch relocations,
including encrypted AES-CTREx data subsections. A virtual NCA accommodates an
expanded RomFS. DLC RomFS views remain alive in the OS, and aoc:u count/list/base
ID/prepare plus FSP OpenDataStorageByDataId expose the running game's DLC.
Installation is disabled during emulation; a full app restart is required for
this native loader/service change.

Two retail format details were checked against local packages: LegalInformation
CNMT records refer to Manual NCAs (type 3), and the retail CNMT footer is an
opaque digest retained from the development build, rather than a checksum of
production CNMT bytes. The fixture suite covers both. Content NCA hashes are
still checked during installation.

## Validation completed

Release build succeeded for `hydra`, `hydra-hot`, `hydra-content-tests`,
`hydra-xci-tests`, and `hydra-shutdown-tests`. The SwiftUI executable was linked
and signed at `build/swiftui-release/bin/Hydra.app/Contents/MacOS/Hydra`.

- `hydra-content-tests build/content-fixtures`: PASS — independent CTREx
  decryption, base/patch overlay, multi-bucket tables, 1,000 random reads,
  retained streams, staging rollback, corruption/missing-content rejection,
  duplicate imports, newest-version selection, bundled/full updates, DLC data
  and required-update handling.
- `hydra-shutdown-tests --check-installed-dlc`: PASS — real IPC decoder,
  count/list/base/prepare, pagination/capacity, missing/foreign IDs, real DLC
  IStorage, and unchanged copied list-event lifetime.
- `hydra-xci-tests build/xci-fixtures`: PASS — existing AES, tickets, malformed
  packages, XCI/NSP/NSZ/XCZ and memory/thread regressions.
- `git diff --check`: clean.

Read-only retail package inspection recognized the local CTR XCI
`0100f9f00c696000`, selected its bundled update version `983040`, loaded its
updated executable/RomFS, read Control display version **1.0.15**, and registered
**7 bundled DLC items**. This did not launch the retail game or verify gameplay
unlock behavior. Two existing NSP bases also passed metadata/filesystem loading;
an existing NSZ still hits the pre-existing sparse/compressed-NCA-section limit.
No retail packages were installed or modified during these checks.

Logs: `build/content-update-build.log`, `build/content-tests.log`,
`build/content-services.log`, `build/content-loader-regression.log`, and
`build/content-ctr-verified.log`.

See [content tests and limits](../../tests/README-content.md) for format limits,
references, storage layout and reproduction commands. Other GPU/performance
worktree edits were left untouched.
