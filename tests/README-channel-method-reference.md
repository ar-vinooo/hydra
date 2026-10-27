# Channel method rejection and MEMBAR visibility

`channel_method_reference.py` audits the Maxwell `clb06f.h` definitions and
checks Hydra's production channel dispatch with real Metal work pending.
Run it on a Mac with Metal:

```sh
ninja -C build/swiftui-release -j 6 hydra hydra-gpu-engine-tests hydra-shutdown-tests
curl -fL https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/host/clb06f.h -o /tmp/hydra-nvidia-clb06f.h
python3 tests/channel_method_reference.py build/swiftui-release/bin/hydra-gpu-engine-tests \
  --nvidia-header /tmp/hydra-nvidia-clb06f.h \
  --report /tmp/hydra-channel-method-reference.json
MTL_DEBUG_LAYER=1 HYDRA_HOT_RELOAD=0 build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-channel-faults
```

The independent Python classification covers 352 cases:

- All 32 MEM_OP_D operation encodings with five high-operand values each.
- Unsupported puller methods, including the explicit ILLEGAL method.
- Engine methods on an unbound subchannel.
- Attempted binding of the channel class B06F as an engine.

The C++ runner encodes an actual Metal blit of 4 KiB and an ordered completion
publisher before each method. Python checks fault status, checkpoint count,
publisher visibility and the first GPU-written word. MEMBAR must wait for
both GPU bytes and the publisher; other covered methods must not force a
checkpoint. Afterward all 4 KiB and the publisher are checked after draining
the GPU. This is real Metal execution, not simulated GPU output.

The PFIFO fault test additionally queues each rejection category followed by a
valid binding/register write. The submission must retire with error 25, leave
that register untouched and omit its successful completion callback. Separate
decode and cross-channel scheduling selectors cover normal execution.

Repeat the Python command with `--control` to expect the old silent-acceptance
policy deliberately; the comparison must fail. Reports record header SHA-256,
the nine audited numeric definitions and two bit fields. The runner enables
Metal validation and disables hot reload.

Source: [NVIDIA Maxwell channel header](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/host/clb06f.h).
This proves encodings and the explicit ILLEGAL method. Rejection of unsupported
operations with error 25 is conservative Hydra policy, not proof of every
hardware fault classification. The five existing accepted TLB/L2 opcodes keep
their current host handling; this fixture does not certify their cache effects
or operand semantics. B06F binding remains unsupported. No physical NVIDIA
device executes, and the instruction stage remains open.
