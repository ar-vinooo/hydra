# Video backend and Host1x diagnostics

`HYDRA_FFMPEG_VIDEO=ON` builds a real software H.264/VP9 decoder using FFmpeg
libavcodec, libavutil and libswscale. This is optional; existing configurations
default to OFF. Set `FFmpeg_ROOT` to the development installation prefix. The
FFmpeg headers and libraries are required only when the option is enabled.

The backend accepts complete codec packets and returns owned NV12 or RGBA8
frames. It handles delayed frames, flush/reset, input padding, colour range and
colour matrix, errors and bounded packet/frame dimensions. Hardware decoding is
not enabled. Dynamic libraries remain external; this does not create a portable
application bundle containing FFmpeg and its dependencies.

Build and run the tests:

```
cmake -S . -B build/swiftui-release -DHYDRA_FFMPEG_VIDEO=ON -DHYDRA_VIDEO_TESTS=ON -DFFmpeg_ROOT=/opt/homebrew/opt/ffmpeg
cmake --build build/swiftui-release --target hydra-video-tests
build/swiftui-release/bin/hydra-video-tests
```

Tests decode independently generated H.264/VP9 vectors and check actual pixel
colours, dimensions, NV12 planes, RGBA alpha, delayed output, frame ownership,
reuse after flush, corrupt packets and input size limits. They also verify the
Host1x submit layout and reject truncated arrays and excessive counts.

## Integration status

Guest H.264 NVDEC execution now resolves mapped Host1x addresses, interprets
SETCLASS/INCR/NONINCR/MASK/IMM command packets, reconstructs SPS/PPS from the
picture context, and writes owned NV12 pictures into guest surfaces. Packet
identity survives FFmpeg reordering. VIC consumes the picture selected by its
luma address and supports a single progressive slot with cropping, NV12/NV21
or RGBA/BGRA output and pitch/block-linear storage. Packed RGB applies the
guest slot color matrix in 10-bit component space, signed coefficients,
translation, soft clamps and planar alpha. A bounded 64 MiB picture cache bridges
decoder and VIC channels and releases pictures when their nvmap is freed.

Video channels have independent nonzero syncpoints. Successful synchronous
submissions return increasing fence thresholds; unsuccessful submissions do not
advance their counters. NvHostCtrl reads and immediate waits use those counters.
Device failures are returned in the nvdrv payload rather than as CMIF errors.
Registered asynchronous NvHostCtrl events signal when their video counter reaches
the requested threshold, and unregister/rearm cancels the earlier subscription.

This remains a partial implementation, **not a claim that Jump Force is fixed**.
VP9 guest header reconstruction, relocations, interlaced field writeback,
multislot VIC composition/scaling, output/gamut matrices,
EOS/reset of guest channels and decode-order surface availability remain work.
FFmpeg can retain reordered pictures after accepting a packet; synchronous
submission completion currently tracks packet acceptance rather than readiness
of every retained picture. Existing GPU syncpoint ID zero retains its old stub
behavior. Video syncpoint IDs are allocated for the lifetime of the System.

With `HYDRA_METAL_CACHE_TESTS=ON`, `hydra-shutdown-tests` additionally submits
independently encoded packets through actual NVDEC ioctls, verifies guest Y/UV
writeback, then submits VIC commands and checks cropping, VU order and both
surface layouts. It verifies returned fences, waits and failed-job completion, plus RGB matrix
conversion, negative coefficients, translation, alpha and channel order in both
surface layouts. Focused modes are `--check-video-writeback` and
`--check-video-events`.

The kernel's condition-variable waiter flag is now published before releasing
the mutex and cleared when signaling removes the final waiter. SDK signaling
can skip the SVC when this flag is zero; omitting it leaves playback/stop worker
threads asleep. The shutdown tests exercise two real HostThread waiters and
partial/full wakeup. Reference:
https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_condition_variable.cpp

## Capture the game's actual submission

```
HYDRA_DUMP_VIDEO=build/swiftui-release/jump-force-video build/swiftui-release/bin/Hydra.app/Contents/MacOS/Hydra > build/swiftui-release/jump-force-opaque.log 2>&1
```

This opt-in trace writes the original submit ioctl, count metadata and the guest
command buffers for NVDEC/VIC. It reads command buffers page by page through the
requesting process MMU and checks each descriptor against its nvmap size. It
does not apply relocations. Execution is handled separately by the channel
interpreter and video engines. Captures are limited to sixteen
submissions and 4 MiB command bytes per submission. The raw ioctl also has a
4 MiB limit. Capturing errors do not change the emulated return value.
Engine execution also captures the selected H.264 picture/bitstream or VIC
configuration, so register addresses can be checked against actual guest data.
It also records up to 128 initial NVDEC/VIC ioctl inputs (64 KiB each), so a
session that stalls before submit still produces initialization diagnostics.

Reference ABI: https://github.com/eden-emulator/mirror/blob/master/src/core/hle/service/nvdrv/devices/nvhost_nvdec_common.h
FFmpeg API: https://ffmpeg.org/doxygen/7.1/group__lavc__decoding.html
