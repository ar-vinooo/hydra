# Buffer queue, vsync, DMA slices, 2D ROP — 2026-10-17

Written after the fact: Codex implemented these and ran out of tokens before documenting them. The tree did not build when picked up; two fixes were needed (`<condition_variable>` missing in `display/driver.hpp`, compile errors in `tests/buffer_queue_tests.cpp` and `tests/blit_2d_tests.cpp`). All 13 test suites pass afterwards with `MTL_DEBUG_LAYER=1`. Not validated in gameplay.

## Buffer queue (services B9–B13)
- `display/binder.*`: slots have an explicit state (Free → Dequeued → Queued → Acquired → Free) and a generation. `MAX_BINDER_BUFFER_COUNT` is 64. `addBuffer`/`queueBuffer`/`cancelBuffer`/`removeBuffer` return bool and reject invalid slots or states.
- Queue state is a `shared_ptr<QueueState>`, so a late host completion cannot free a slot of a newly connected producer (generation check).
- `Binder::releaseCallback(slot)` is attached to the present command buffer (`Layer::acquirePresentTexture`): the slot becomes Free (and the event signals) only when the host has finished with it.
- `getBuffer` returns a copy (`std::optional`); `IHOSBinderDriver::RequestBuffer` answers `BadValue` for unknown slots.
- Test: `hydra-buffer-queue-tests` (`testQueue`).

## Vsync (B14)
- `display/vsync_schedule.hpp`: fixed 60 Hz guest vblank, deadlines computed from an origin (no drift), skips missed vblanks after suspension.
- `Driver::startVSync/stopVSync` run a `jthread`; `OS` starts it after the servers and stops it (then `shutdownQueues`) in `shutdownServices`.
- `dispdrv` moved to its own `display_server` so binder waits cannot block the other services.
- Test: `hydra-buffer-queue-tests` (`testVSyncSchedule`, 60 ticks/s at any host rate).

## DMA slices
- `engines/copy.cpp`: LINE_COUNT may span several Z slices (`sliceRow`/`pitchRow`), LAYER is the first slice, rectangles are bounded by available slices and rejected when they cross the X edge or overflow; unmapped base addresses are skipped with a warning.
- Test: `hydra-dma-slice-tests`.

## 2D ROP / slices
- `engines/2d.cpp`: binary ROP truth tables on raw (non-filtered) copies, `rawBlit2D` for overlapping copies with atomic bounds rejection, pitch and block-linear volume slices, point sampling.
- Other operations/ROP codes are rejected with a log, not silently treated as SRCCOPY.
- Tests: `tests/blit_2d_tests.cpp` (in `hydra-gpu-engine-tests`).

## Still open here
- Fence/syncpoint-accurate release (the release is host-command-buffer completion, GPU fences stay behind `HYDRA_GPU_FENCES`).
- DMA remap + slices combinations; 2D non-SRCCOPY operations beyond ROP binary tables.
