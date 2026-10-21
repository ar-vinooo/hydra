# Guest event stream

This implementation records the observed guest timeline separately from
host-resolved `.hcap` command snapshots. The event replay reconstructs memory
observations, ownership epochs and ordering dependencies. The Python semantic
replay does not render. The separate native replay described below feeds consumed
PFIFO words through Hydra's production decoder and engines on Metal. `.hcap`
remains available for rendering captured host commands independently of PFIFO.

## Capture

Set these variables before starting Hydra:

```powershell
$env:HYDRA_GUEST_EVENTS = 'C:\captures\guest-events.jsonl'
$env:HYDRA_GUEST_EVENTS_LIMIT_MB = '256'
```

Choose an unused output path in an existing directory. The optional budget
defaults to 256 MiB and includes hexadecimal memory payloads. Capture is disabled
when the environment variable is absent. An invalid configuration, output error
or exceeded budget stops recording; budget truncation emits a terminal marker.
An interrupted file without a footer is incomplete.

Each JSONL record has schema 1 and a contiguous order number. Origins identify
the channel, method sequence and source address. Recorded event families include:

| Events | Recorded information |
| --- | --- |
| Channel and address-space lifecycle | Channel switches, mapping changes, physical backing aliases and destruction |
| PFIFO | Entries, consumed words, dispatched methods and macro flushes |
| Semaphore and syncpoint | Wait dependencies, observed successful values, resumes and releases |
| Draw, clear, blit, compute and DMA | Operation origins and conservative resource access manifests |
| Memory | Initial mapped bytes, notified CPU writes, report publication and texture writeback observations |
| GPU work | Issued write epochs, completion status and explicit checkpoints |
| Native Metal submissions | Unique submission IDs, command-buffer identity, wrapper identity and completion status |
| Reports | Queued report tokens and completion/publication outcomes |

Memory payloads use chunks of at most 64 KiB. Snapshot reads occur outside the
recorder mutex because reading a protected guest page can trigger GPU writeback.
Capturing memory can affect timing. CPU changes are observed through the existing
invalidation hooks; the recorder is not an independent interceptor for every CPU
store or allocation lifetime. Physical pointer reuse without a recorded write
cannot establish a new allocation's contents.

The footer includes outstanding GPU epochs, native submissions and reports. It does not force a GPU
flush. Pending work, unmatched waits or truncation make replay incomplete.

## Semantic replay

The standalone script needs Python 3.10 or newer and no third-party packages:

```powershell
python tools/replay_guest_events.py C:\captures\guest-events.jsonl --output C:\captures\summary.json
```

The replay reconstructs sparse physical memory shared by aliased mappings. GPU
writes make produced bytes unknown until a recorded publication or writeback
supplies them. A late GPU completion updates its token without restoring an older
owner over a newer CPU write. Initial snapshots fill unobserved ranges without
overwriting previously recorded ownership.

The script rejects malformed ordering, unmatched completions, inconsistent
mapping lifecycles, methods overtaking blocked channels and unsatisfied recorded
semaphore predicates. Where memory bytes are known, it compares successful
acquires with the reconstructed value. The summary includes stale read
observations, dependency counts, pending/failed work and a memory observation
checksum; it always reports `rendered: false`.

`--allow-incomplete` permits inspecting a prefix while retaining its incomplete
status. `--strict` also rejects stale reads, recorded failures, unresolved acquire
values and streams without guest methods. These checks describe captured
evidence; they do not establish hardware equivalence or complete race coverage.

## Native PFIFO replay

Export a structurally complete stream, then use the macOS executable:

```sh
python3 tools/replay_guest_events.py guest-events.jsonl --native-output guest-events.hge
build/swiftui-release/bin/hydra-guest-replay guest-events.hge --output native.json
```

`hydra-guest-replay` is a new CMake target beside `hydra-gpu-replay`. The `.hge`
transport is explicitly little endian, versioned, bounded and independent of C++
struct layout. The native reader checks record ordering, payload lengths and
session boundaries. `--budget-mb` bounds event storage and backing allocations
separately (default 512 MiB each).

The native tool reconstructs address spaces with one allocation for overlapping
physical ranges, restores initial bytes without overwriting existing producers,
and keeps allocations until GPU work and report callbacks have drained. Each
channel has independent production decoder, macro and engine state. Deferred
word execution permits mapping snapshots captured between fetch and dispatch to
be restored before execution. Decoded method, argument, sequence, source and
subchannel must match the captured method events.

CPU inputs without a channel are applied with cache invalidation. Channel-owned
DMA/inline/release writes, published reports and GPU texture writebacks are
compared against actual replay output; they are not copied in as expected GPU
results. Failed methods, unsupported dependencies, unknown read inputs and
unfinished packets fail the run. Initial allocated syncpoint values are recorded
and restored. A later external producer not represented by captured CPU inputs
or GPU methods cannot be invented; such a dependency can fail replay.

The native replay follows the captured word/channel interleave and uses
conservative checkpoints. It exercises the production decoder and engines, but
does not recreate guest CPU execution, the original asynchronous scheduler timing
or unobserved races. Queries with nondeterministic timestamps can differ; byte
comparisons report the difference rather than silently accepting it. Captures
must begin before channel initialization; an arbitrary prefix of earlier engine
state is not synthesized. Missing/truncated inputs are not valid references.

## Differential comparison and ordering stress

```sh
python3 tools/compare_gpu_replays.py reference.json native.json --output comparison.json
python3 tools/stress_gpu_ordering.py guest-events.hge \
  --replay build/swiftui-release/bin/hydra-guest-replay \
  --scheduler build/swiftui-release/bin/hydra-shutdown-tests \
  --iterations 1000 --seed 1 --output-dir build/correctness/ordering-run-1
```

The comparator requires nonempty successful observations and compares native
method/word/event counts and each observation's location, length and byte hash.
An independently collected hardware reference can use the same summary schema
and `--reference-kind hardware`; the label does not collect or authenticate device
evidence. No real-device result is included with this change.

The stress driver compares deterministic replay outputs across per-word delay
seeds, then invokes `--check-gpu-ordering-stress <iterations> <seed>` on the real
PFIFO worker. That scheduler scenario waits across channels, performs an inline
producer transfer, overwrites it with real Metal colour clears on alternating
iterations, runs the DMA consumer through a partial physical alias, and checks
a later CPU overwrite. It varies producer admission delay. Native compute/image
completion cases remain in the GPU-engine suite. The driver writes logs, output
summaries and failure status, including timeouts. Its output directory must be
new so old results cannot be mistaken for a fresh pass.

The source includes `tests/guest_event_tests.py` for completion/footer, partial
ownership, semaphore wrap, malformed ordering, export and differential failure
cases. These additions are wired into the existing optional validation script.

## Status

Builds and runtime validation of this continuation are deferred at the user's
request. The native replay, comparator and stress sources are implemented but
unrun. Real-device evidence remains external. Barrier reduction and batching
changes are deferred until P0 correctness checks pass, as required by the report;
no unverified performance mode is enabled by these additions.
