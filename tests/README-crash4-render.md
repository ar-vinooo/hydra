# Crash Bandicoot 4 rendering regressions

The Metal regression tests cover the following renderer fixes:

- Default omitted vertex position/output components to `(0, 0, 0, 1)`, while preserving explicit guest writes.
- Decode ICMP/FCMP operand variants, predicates, ordered/unordered NaN comparisons and FCMP comparisons with subnormal inputs/FTZ.
- Preserve SHL/SHR clamp and wrap semantics, signed right shifts and bit reversal. Extended modes and condition codes remain unsupported.
- Decode BFI bitfield insertion and SHF funnel shifts with bounded shift counts. Condition-code outputs and SHF extended modes remain unsupported.
- Keep predicate-only instructions from shadowing general-purpose registers during bindless texture tracking, and track every word loaded by LDC.B128.
- Decode bounded BRX switches consisting of an unsigned IMNMX clamp, optional immediate SHL and optional indexed LDC jump table. Enumerate validated targets and include the table words in guest shader cache keys. Other indirect branch patterns remain unsupported.
- Sample GPU-written Z24S8 depth through its G24R8 texture alias.
- Synchronize overlapping buffer texture subranges through Metal buffer copies, including cross-format R16/R32 aliases, byte-offset copies, aliases created after a GPU write and CPU uploads followed by newer GPU writes. Apply overlapping GPU writes in time order.

Build and run on macOS with a Metal GPU:

```sh
cmake --build build/swiftui-release --target hydra hydra-shader-library-tests hydra-texture-cache-tests hydra-raster-state-tests hydra-render-hash-tests -j 6
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-shader-library-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-texture-cache-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-raster-state-tests
build/swiftui-release/bin/hydra-render-hash-tests
```

The shader and texture tests execute actual Metal commands and inspect results. Passing these tests does not establish correct rendering for every game shader. Crash 4 gameplay still needs a visual check on the combined build; capture stdout/stderr when launching Hydra and keep heavy per-draw tracing disabled for that check.
