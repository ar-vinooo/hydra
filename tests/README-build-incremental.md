# Incremental GPU builds

The source-bound GPU optimization fingerprint is defined only while compiling
`optimization_policy.cpp`. Applying it to all of `hydra-core` changed every
compiler command after any covered GPU/CPU-memory/test/tool edit, invalidating
the core precompiled header and hundreds of unrelated objects.

The policy source uses its own compile definitions and skips the core PCH.
Fingerprint calculation, stale-receipt rejection, hot-reload restrictions and
the Python/CMake source normalization rules are retained. Migrating existing
objects off the old target-wide flags requires one rebuild. Later `.cpp` edits
rebuild the affected object and policy, followed by the necessary archive/link
steps. Shared header edits still rebuild their consumers.

```sh
python3 -B tests/build_incremental_tests.py
```

The test builds a small CMake/Ninja project using the production policy, gate
and receipt probe. It checks that only the policy compiler command receives the
fingerprint, an unrelated source edit preserves the PCH timestamp, an old
receipt is rejected after rebuilding, a new one is accepted, and hot reload
disables the policy without rebuilding the PCH.

Existing debug workflows can also use `python3 tools/hot_debug.py watch` to
rebuild supported GPU implementations as a hot module. Header/layout changes
continue to require a full app build and restart.

Run only one build process in a build directory. Parallel builds must use
separate directories; concurrent Ninja writers can corrupt `.ninja_deps` and
cause repeated recompilation. A damaged generated dependency log can be backed
up and repaired with `ninja -C build/swiftui-release -t recompact`, followed by
one rebuilding pass. Confirm a subsequent unchanged build reuses the objects.
See the concurrent-writer checks and recovery code in
[Ninja's dependency log implementation](https://github.com/ninja-build/ninja/blob/v1.13.2/src/deps_log.cc).
