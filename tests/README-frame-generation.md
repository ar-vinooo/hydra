# Frame generation output rate

Native rendering and generated output have independent settings in Graphics:

```toml
[Graphics]
frame_rate_limit = 60
frame_generation = "Interpolate" # "Off", "Interpolate", "MetalFX"
frame_generation_rate_mode = "Multiple" # "Variable" or "Multiple"
frame_generation_target_fps = 120
frame_generation_multiplier = 2
```

`frame_rate_limit` controls native game pacing and applies when a game boots.
FG settings apply immediately:

- **Variable:** targets `frame_generation_target_fps`, regardless of native FPS.
  Targets below native FPS add no frames and do not throttle the game.
- **Multiple:** targets current native FPS times `frame_generation_multiplier`.
  With a native limit of 60 and multiplier 2, 60 native FPS targets 120 output
  FPS; if native performance drops to 45 FPS, output targets 90 FPS. The native
  FPS measurement excludes generated frames and updates about once per second.

Both numeric settings accept positive integers. There is no screen refresh
clamp or 8x interpolation limit. Before native FPS has been measured, Multiple
uses 60 native FPS. Missing FG rate settings default to Variable, 120 FPS, 2x.

SwiftUI uses a separate presentation timer while FG is enabled; SDL paces its
presentation loop using the same output-rate calculation. macOS drawable vsync
is disabled while FG is active and restored when FG is off. Interpolation uses
elapsed time and output steps instead of display-refresh counts. Identical
interpolation steps reuse their generated image. Long pauses invalidate stale
motion using elapsed time rather than a fixed count of refreshes.

These are requested generation/presentation rates. GPU speed, timer scheduling,
drawable availability and platform presentation behavior can reduce achieved
throughput. The existing window FPS counter still reports images confirmed by
drawable presentation callbacks, rather than the requested FG rate.

## Verification

```sh
cmake --build build/swiftui-release --target hydra hydra-frame-generation-tests -j 6
build/swiftui-release/bin/hydra-frame-generation-tests
```

The timing suite checks native/output independence, Variable and Multiple,
dynamic native FPS, uncapped 240/480 FPS requests, all 16 interpolation steps
at 16x, the exact MetalFX midpoint, missed deadlines and invalid zero values.
It does not measure achieved GPU throughput or validate generated image quality.

For a manual runtime check, use a game reaching 60 native FPS, set the native
limit to 60 and FG Multiple to 2. Confirm native FPS stays near 60 and the
requested output is 120, including on a 60 Hz display. Then switch Variable to
an arbitrary target such as 237; verify that the native setting stays at 60.
Check MetalFX midpoint generation, switching FG off, and closing/reopening the
emulation view. Actual displayed FPS can differ from the requested rate.
