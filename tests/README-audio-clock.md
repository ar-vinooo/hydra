# Audio timeline and underrun recovery checks

The Cubeb callback counts all requested output frames, including silence when
the PCM queue is empty. `consumedSampleClock` excludes missing frames since the
current renderer start when deciding how much PCM to enqueue. Replacing those
missing frames as if they had drained the queue previously accumulated host
queue latency. Backpressure now applies before voices advance, so full queues
cannot silently discard mixed voice frames.

Guest renderer time is tracked separately. `elapsedDspFrames` follows the
uncorrected device clock, with the same eight-frame startup lead, rather than
the PCM enqueue counter. Host underruns must not permanently subtract time from
the counter reported in the renderer update response. This is a proposed fix
for the observed failure to recover after a background FPS drop; gameplay
recovery must still be verified.

Run `hydra-audio-tests` with `HYDRA_AUDIO_TESTS=ON`. It covers PCM decoding and
FIFO ownership, sample-clock interpolation, underrun origin/saturation bounds,
separation of guest time from missing host samples, and 20,000 simulated callback
cycles with repeated underruns. The simulation keeps the PCM queue at its target
without cumulative latency growth.

For the hardware test, run Hydra with `HYDRA_DUMP_AUDIO=<diagnostic-directory>`.
The timing log includes raw hardware clock, produced PCM, missing host samples,
queued samples, consumed PCM clock, guest DSP frames and valid guest waves.
Test stable gameplay, a prolonged Alt-Tab, and return to the same scene until FPS
recovers. Success requires normal tempo after recovery, stable queue depth and
restored voice supply. Unit tests alone do not establish audible recovery.
