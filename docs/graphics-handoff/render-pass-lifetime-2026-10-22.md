# Render-pass descriptor lifetime — 22 October 2026

The reported CTR startup force-close was reproduced under LLDB. It is a native
EXC_BAD_ACCESS/SIGSEGV in `objc_msgSend`, called by `continuesRenderPass` at its
first `renderTargetWidth` read, through `OcclusionCounter::prepareDraw` and
`Renderer::drawHot38`. The cached pointer to the open pass's Metal descriptor
had outlived the descriptor's owner.

`CommandBuffer` now retains the original descriptor when creating a render
encoder and when adopting a compatible descriptor during a merge (retaining the
new descriptor before releasing the previous one). `endEncodingHot6` releases
and clears that reference. Keeping the original descriptor also preserves the
existing same-descriptor fast path and its clear/load semantics.

The new `pass-lifetime` test releases clear/load/load descriptors immediately
after encoding each draw. Before the fix, NSZombieEnabled catches a call to
`renderTargetWidth` on a deallocated descriptor. With the fix and Metal API
Validation enabled, it reports 12,288 samples and 2 merged passes. With merging
disabled it reports the same 12,288 samples and 0 merges. The existing
`occlusion-merge` test remains at 8,192 samples and 1 merge.

Debugging evidence and build logs are in `build/game-crash-console.log`,
`build/pass-lifetime-before.log`, `build/pass-lifetime-after.log`,
`build/pass-lifetime-no-merge.log`, `build/pass-lifetime-occlusion.log`, and
`build/pass-lifetime-hot-build.log`. No shader caches were deleted and no
performance settings were changed in the user's configuration.

## Final verification

The normal CMake targets `hydra-hot`, `hydra`, and `hydra-metal-cache-tests`
rebuilt successfully. The final test executable passed `pass-lifetime` with
NSZombieEnabled + Metal validation, both with merging enabled (12,288 samples,
2 merges) and disabled (12,288 samples, 0 merges), and the existing
`occlusion-merge` case (8,192 samples, 1 merge).

The rebuilt SwiftUI app was launched normally and CTR was opened from the game
library. It passed the former approximately three-second startup crash, rendered
the animated intro, and the window reported 60 FPS. The running process was left
open for the user. This verifies startup rendering; a full race/session was not
part of this check. Final logs: `build/pass-lifetime-app-build.log`,
`build/pass-lifetime-final*.log`, and `build/game-crash-fixed-console.log`.
