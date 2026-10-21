"""Binary transport for native guest replay (little endian, version 1).

The source JSONL remains the canonical capture. Each record is a uint32 kind,
uint32 payload length, nine uint64 fields (order, channel, sequence, source,
address, size, version, value, auxiliary), followed by raw payload bytes.
Keep KIND_NAMES aligned with tests/guest_replay_format.hpp.
"""
import struct

MAGIC = b"HYDRAE1\n"
KIND_NAMES = (
    "session_begin", "session_end", "map", "unmap", "channel_switch",
    "channel_destroy", "address_space_destroy", "pfifo_entry", "pfifo_word",
    "method", "macro_flush", "semaphore_wait", "semaphore_resume",
    "syncpoint_wait", "syncpoint_resume", "semaphore_acquire",
    "semaphore_acquired", "semaphore_release", "semaphore_release_queued",
    "memory_initial", "memory_write", "memory_published", "memory_writeback",
    "gpu_write_issued", "gpu_complete", "report_queued", "report_complete",
    "resource_read", "resource_write", "resource_invalidate", "checkpoint",
    "draw", "clear", "blit", "compute", "dma_access", "dma_copy", "channel_fault",
    "syncpoint_initial",
    "native_submit", "native_complete",
)
KINDS = {name: index for index, name in enumerate(KIND_NAMES)}
FIELDS = ("order", "channel", "sequence", "source", "address", "size",
          "version", "value", "auxiliary")
HEADER = struct.Struct("<II9Q")


def write_record(destination, event):
    name = event["event"]
    if name not in KINDS:
        raise ValueError(f"event {name!r} cannot be exported for native replay")
    payload = bytes.fromhex(event.get("data", ""))
    if len(payload) > 65536:
        raise ValueError("native event payload exceeds 64 KiB")
    destination.write(HEADER.pack(KINDS[name], len(payload),
                                 *(event.get(key, 0) for key in FIELDS)))
    destination.write(payload)
