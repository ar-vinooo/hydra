"""Generate tiny synthetic cartridges and independent AES test fixtures.

Requires Python cryptography and the zstd CLI (tests only); contains no
console keys or games.
"""
from pathlib import Path
import random
import struct
import subprocess
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


def zstd(data):
    return subprocess.run(["zstd", "-q", "-c"], input=bytes(data),
                          stdout=subprocess.PIPE, check=True).stdout


def ncz(stored, plain, sections, block_exponent=None):
    """NCZ as written by nsz: the stored (encrypted) first 0x4000 bytes, a
    section table, then the decrypted rest of the NCA compressed with zstd."""
    body = bytes(plain[0x4000:])
    out = bytearray(stored[:0x4000]) + b"NCZSECTN" + struct.pack("<Q", len(sections))
    for offset, size, crypto in sections:
        out += struct.pack("<QQQQ", offset, size, crypto, 0) + bytes(32)
    if block_exponent is None:
        return out + zstd(body)
    size = 1 << block_exponent
    blocks = []
    for i in range(0, len(body), size):
        compressed = zstd(body[i:i + size])
        blocks.append(compressed if len(compressed) < len(body[i:i + size]) else body[i:i + size])
    out += b"NCZBLOCK" + bytes((2, 1, 0, block_exponent)) + struct.pack("<IQ", len(blocks), len(body))
    out += b"".join(struct.pack("<I", len(block)) for block in blocks)
    return out + b"".join(blocks)


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
    (directory / "base.nsp").write_bytes(partition([("base.nca", program)], False))
    (directory / "BASE.NSP").write_bytes(partition([("base.nca", program)], False))
    (directory / "update-only.nsp").write_bytes(partition([("update.nca", update)], False))
    (directory / "multiple-base.nsp").write_bytes(partition([("one.nca", program), ("two.nca", program)], False))
    (directory / "truncated.nsp").write_bytes(b"PFS0")
    bad_nsp = bytearray(partition([("base.nca", program)], False))
    put(bad_nsp, 16, "Q", 2**64 - 1)
    (directory / "bad-offset.nsp").write_bytes(bad_nsp)
    (directory / "no-program.nsp").write_bytes(partition([("notes.txt", b"test")], False))
    ticket = bytearray(0x2c0)
    put(ticket, 0, "I", 0x010004)
    ticket[0x180:0x190] = bytes(range(16))
    ticket[0x2a0:0x2b0] = bytes(range(16, 32))
    (directory / "ticket.nsp").write_bytes(partition([("base.nca", program), ("common.tik", ticket)], False))
    conflicting = bytearray(ticket)
    conflicting[0x180] ^= 1
    (directory / "conflicting-ticket.nsp").write_bytes(partition([("base.nca", program), ("one.tik", ticket), ("two.tik", conflicting)], False))
    (directory / "bad-ticket.nsp").write_bytes(partition([("base.nca", program), ("common.tik", ticket[:0x180])], False))

    program32 = bytearray(program)
    # Locate the actual NPDM in the generated ExeFS rather than depending on
    # its string-table length/alignment.
    npdm_start = program32.find(b"META", 0xc00)
    program32[npdm_start + 0xc] = 0
    (directory / "32-bit.nsp").write_bytes(partition([("base.nca", program32)], False))

    # NCZ: an encrypted NCA whose CTR section is stored decrypted, in block
    # (with a stored incompressible block and a short last block) and solid form.
    ncz_plain = bytearray(0x3be00)
    ncz_plain[:0xc00] = plain[:0xc00]
    put(ncz_plain, 0x208, "Q", len(ncz_plain))
    ncz_plain[0x240:0x280] = bytes(0x40)
    noise = random.Random(1).randbytes(0x8000)
    for i, (start, end, kind) in enumerate(((0x4000, 0x34000, 3), (0x34000, 0x3be00, 1))):
        put(ncz_plain, 0x240 + i * 16, "II", start // 512, end // 512)
        fs = 0x400 + i * 0x200
        ncz_plain[fs + 4] = kind
        put(ncz_plain, fs + 0x140, "II", 7, 11)
        ncz_plain[start:end] = bytes((j // 7 + i * 37) % 256 for j in range(end - start))
    ncz_plain[0x10000:0x18000] = noise
    ncz_stored = bytearray(ncz_plain)
    counter = bytes(ncz_plain[0x540:0x548][::-1]) + (0x4000 // 16).to_bytes(8, "big")
    ncz_stored[0x4000:0x34000] = encrypt(section_keys[32:48], modes.CTR(counter), ncz_plain[0x4000:0x34000])
    for i in range(6):
        start = i * 512
        ncz_stored[start:start + 512] = encrypt(header_key, modes.XTS(i.to_bytes(16, "big")), ncz_plain[start:start + 512])
    ncz_sections = [(0x4000, 0x30000, 3), (0x34000, 0x7e00, 1)]
    (directory / "ncz-expected.nca").write_bytes(ncz_plain)
    (directory / "ncz-block.ncz").write_bytes(ncz(ncz_stored, ncz_plain, ncz_sections, 14))
    (directory / "ncz-solid.ncz").write_bytes(ncz(ncz_stored, ncz_plain, ncz_sections))

    # NSZ/XCZ: the base program relocated past the uncompressed NCZ head.
    program_ncz = bytearray(0x4400)
    program_ncz[:0xc00] = program[:0xc00]
    put(program_ncz, 0x208, "Q", len(program_ncz))
    put(program_ncz, 0x240, "II", 0x20, 0x21)
    put(program_ncz, 0x250, "II", 0x21, 0x22)
    program_ncz[0x4000:0x4400] = program[0xc00:0x1000]
    program_sections = [(0x4000, 0x200, 1), (0x4200, 0x200, 1)]
    base_ncz = ncz(program_ncz, program_ncz, program_sections, 14)
    (directory / "base.nsz").write_bytes(partition([("base.ncz", base_ncz)], False))
    (directory / "BASE.NSZ").write_bytes(partition([("base.ncz", base_ncz)], False))
    (directory / "base.xcz").write_bytes(cartridge([("base.ncz", base_ncz)]))
    bad_ncz = bytearray(base_ncz)
    bad_ncz[0x4000:0x4008] = b"BADMAGIC"
    (directory / "bad-magic.nsz").write_bytes(partition([("base.ncz", bad_ncz)], False))
    (directory / "truncated.nsz").write_bytes(partition([("base.ncz", base_ncz[:0x4020])], False))

    # Independent title-key AES-ECB + AES-CTR fixture (synthetic keys only).
    title_kek = bytes(range(128, 144))
    title_key = bytes(range(144, 160))
    rights_id = bytes(range(16, 32))
    rights_plain = bytearray(plain)
    rights_plain[0x230:0x240] = rights_id
    rights_plain[0x240:0x280] = bytes(0x40)
    put(rights_plain, 0x240, "II", 0xc00 // 512, 0x1000 // 512)
    rights_plain[0x300:0x340] = bytes(0x40)
    rights_plain[0x404] = 3
    rights_encrypted = bytearray(rights_plain)
    counter = bytes(rights_plain[0x540:0x548][::-1]) + (0xc00 // 16).to_bytes(8, "big")
    rights_encrypted[0xc00:0x1000] = encrypt(title_key, modes.CTR(counter), rights_plain[0xc00:0x1000])
    for i in range(6):
        start = i * 512
        rights_encrypted[start:start + 512] = encrypt(header_key, modes.XTS(i.to_bytes(16, "big")), rights_plain[start:start + 512])
    (directory / "rights-encrypted.nca").write_bytes(rights_encrypted)
    (directory / "rights-expected.nca").write_bytes(rights_plain)
    with (directory / "prod.keys").open("a") as f:
        f.write("titlekek_00 = " + title_kek.hex() + "\n")
        f.write("encrypted_titlekey_" + rights_id.hex() + " = " + encrypt(title_kek, modes.ECB(), title_key).hex() + "\n")
    print("Generated synthetic XCI/NSP/AES fixtures in", directory)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
