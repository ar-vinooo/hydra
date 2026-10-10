"""Generate tiny synthetic cartridges and independent AES test fixtures.

Requires Python cryptography (tests only); contains no console keys or games.
"""
from pathlib import Path
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def put(data, offset, fmt, *values):
    struct.pack_into("<" + fmt, data, offset, *values)


def encrypt(key, mode, data):
    cryptor = Cipher(algorithms.AES(key), mode).encryptor()
    return cryptor.update(bytes(data)) + cryptor.finalize()


def partition(files, hashed):
    names = b""
    entries = bytearray()
    content = b""
    for name, data in files:
        entry = struct.pack("<QQII", len(content), len(data), len(names), 0)
        if hashed:
            entry += bytes(40)
        entries.extend(entry)
        names += name.encode() + b"\0"
        content += bytes(data)
    return (b"HFS0" if hashed else b"PFS0") + struct.pack("<III", len(files), len(names), 0) + entries + names + content


def cartridge(files):
    secure = partition(files, True)
    root = partition([("secure", secure)], True)
    header = bytearray(0x200)
    header[0x100:0x104] = b"HEAD"
    put(header, 0x130, "Q", 0x200)
    return header + root


def main(directory):
    directory.mkdir(parents=True, exist_ok=True)
    header_key = bytes(range(32))
    area_key = bytes(range(32, 48))
    section_keys = bytes(range(64, 128))
    (directory / "prod.keys").write_text(
        "# Synthetic test keys\nheader_key = " + header_key.hex() +
        "\nkey_area_key_application_00 = " + area_key.hex() + "\n")

    plain = bytearray(0x1800)
    plain[0x200:0x204] = b"NCA3"
    plain[0x205] = 0
    plain[0x206] = 1
    put(plain, 0x208, "Q", len(plain))
    plain[0x300:0x340] = encrypt(area_key, modes.ECB(), section_keys)
    for i, kind in enumerate((3, 2, 1)):
        start = 0xC00 + i * 0x400
        put(plain, 0x240 + i * 16, "II", start // 512, (start + 0x400) // 512)
        fs = 0x400 + i * 0x200
        plain[fs + 4] = kind
        put(plain, fs + 0x140, "II", 7, 11)
        plain[start:start + 0x400] = bytes((j * 19 + i * 37) % 256 for j in range(0x400))
    encrypted = bytearray(plain)
    counter = bytes(plain[0x540:0x548][::-1]) + (0xC00 // 16).to_bytes(8, "big")
    encrypted[0xC00:0x1000] = encrypt(section_keys[32:48], modes.CTR(counter), plain[0xC00:0x1000])
    for i in range(2):
        start = 0x1000 + i * 512
        encrypted[start:start + 512] = encrypt(section_keys[:32], modes.XTS(i.to_bytes(16, "big")), plain[start:start + 512])
    for i in range(6):
        start = i * 512
        encrypted[start:start + 512] = encrypt(header_key, modes.XTS(i.to_bytes(16, "big")), plain[start:start + 512])
    (directory / "encrypted.nca").write_bytes(encrypted)
    (directory / "expected.nca").write_bytes(plain)

    # Independent CTR fixture spanning cache windows and a counter change.
    large = bytearray(0x52000)
    large[:0xc00] = plain[:0xc00]
    put(large, 0x208, "Q", len(large))
    large[0x240:0x280] = bytes(0x40)
    for i, (start, end) in enumerate(((0xc00, 0x32000), (0x32000, 0x52000))):
        put(large, 0x240 + i * 16, "II", start // 512, end // 512)
        fs = 0x400 + i * 0x200
        large[fs + 4] = 3
        put(large, fs + 0x140, "II", 7 + i, 11 + i)
        large[start:end] = bytes((j * 19 + i * 37) % 256 for j in range(end - start))
    large_encrypted = bytearray(large)
    for i, (start, end) in enumerate(((0xc00, 0x32000), (0x32000, 0x52000))):
        fs = 0x400 + i * 0x200
        counter = bytes(large[fs + 0x140:fs + 0x148][::-1]) + (start // 16).to_bytes(8, "big")
        large_encrypted[start:end] = encrypt(section_keys[32:48], modes.CTR(counter), large[start:end])
    for i in range(6):
        start = i * 512
        large_encrypted[start:start + 512] = encrypt(header_key, modes.XTS(i.to_bytes(16, "big")), large[start:start + 512])
    (directory / "large-ctr.nca").write_bytes(large_encrypted)
    (directory / "large-ctr-expected.nca").write_bytes(large)

    npdm = bytearray(0x80)
    npdm[:4] = b"META"
    npdm[0xC] = 1
    npdm[0xE] = 44
    put(npdm, 0x1C, "I", 0x4000)
    npdm[0x20:0x27] = b"Fixture"
    nso = bytearray(0x100)
    nso[:4] = b"NSO0"
    code = partition([("main.npdm", npdm), ("main", nso)], False)
    assert len(code) <= 512
    program = bytearray(0x1000)
    program[0x200:0x204] = b"NCA3"
    put(program, 0x208, "Q", len(program))
    put(program, 0x210, "Q", 0x0100000000001000)
    put(program, 0x240, "II", 6, 7)
    put(program, 0x250, "II", 7, 8)
    program[0x402:0x405] = bytes((1, 2, 1))
    put(program, 0x42C, "I", 2)
    put(program, 0x440, "QQ", 0, 512)
    program[0x602:0x605] = bytes((0, 3, 1))
    program[0x608:0x60C] = b"IVFC"
    put(program, 0x614, "I", 7)
    put(program, 0x690, "QQ", 0, 512)
    program[0xC00:0xC00 + len(code)] = code
    put(program, 0xE00, "Q", 0x50)
    base = cartridge([("base.nca", program)])
    (directory / "base.xci").write_bytes(base)
    readonly = directory / "readonly.xci"
    readonly.unlink(missing_ok=True)
    readonly.write_bytes(base)
    readonly.chmod(0o444)
    # An update with the same title ID must not displace the base game.
    update = bytearray(program)
    update[0x604] = 4
    (directory / "base-with-update.xci").write_bytes(cartridge([("update.nca", update), ("base.nca", program)]))
    (directory / "update-only.xci").write_bytes(cartridge([("update.nca", update)]))
    (directory / "multiple-base.xci").write_bytes(cartridge([("one.nca", program), ("two.nca", program)]))
    (directory / "truncated.xci").write_bytes(b"HEAD")
    bad = cartridge([("base.nca", program)])
    bad[0x100:0x104] = b"BAD!"
    (directory / "bad-magic.xci").write_bytes(bad)
    bad = cartridge([("base.nca", program)])
    put(bad, 0x130, "Q", 2**64 - 1)
    (directory / "bad-offset.xci").write_bytes(bad)
    bad = cartridge([("base.nca", program)])
    put(bad, 0x220, "I", 2**32 - 1)
    (directory / "bad-name.xci").write_bytes(bad)
    (directory / "bad-section.xci").write_bytes(cartridge([("base.nca", program[:0xC00])]))
    broken = bytearray(program)
    broken[0xC00:0xC04] = b"BAD!"
    (directory / "bad-exefs.xci").write_bytes(cartridge([("base.nca", broken)]))
    (directory / "not-a-game").write_bytes(b"test")
    print("Generated synthetic XCI/AES fixtures in", directory)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
