#!/usr/bin/env python3
"""Independent synthetic CNMT, DLC and encrypted BKTR fixtures (no game data)."""
import hashlib
import pathlib
import struct
import sys
sys.dont_write_bytecode = True
from generate_xci_fixtures import put, partition, cartridge, encrypt
from cryptography.hazmat.primitives.ciphers import modes

APP = 0x0100000000002000
DLC = APP + 0x1001
HEADER_KEY = bytes(range(32))
AREA_KEY = bytes(range(32, 48))
SECTION_KEYS = bytes(range(64, 128))


def nca(content_type, title, sections):
    result = bytearray(0xc00)
    result[0x200:0x204] = b'NCA3'
    result[0x205] = content_type
    put(result, 0x210, 'Q', title)
    for i, (fs, data) in enumerate(sections):
        start = len(result)
        result += data
        result += bytes((-len(result)) % 512)
        put(result, 0x240 + 16*i, 'II', start//512, len(result)//512)
        result[0x400 + i*0x200:0x600 + i*0x200] = fs
    put(result, 0x208, 'Q', len(result))
    return result


def pfs_header(size):
    fs = bytearray(512)
    fs[2:5] = bytes((1, 2, 1))
    put(fs, 0x2c, 'I', 2)
    put(fs, 0x40, 'QQ', 0, size)
    return fs


def rom_header(size):
    fs = bytearray(512)
    fs[2:5] = bytes((0, 3, 1))
    fs[8:12] = b'IVFC'
    put(fs, 0x14, 'I', 7)
    put(fs, 0x90, 'QQ', 0, size)
    return fs


def table(entries, end, relocation):
    stride = 20 if relocation else 16
    result = bytearray(0x8000)
    put(result, 0, 'IIQQ', 0, 1, end, 0)
    put(result, 0x4000, 'IIQ', 0, len(entries), end)
    for i, entry in enumerate(entries):
        put(result, 0x4010 + i*stride, 'QQI' if relocation else 'QII', *entry)
    return result


def packaged_meta(kind, title, version, records, required=0):
    ext = 0x18 if kind == 0x81 else 0x10
    result = bytearray(0x20 + ext)
    put(result, 0, 'QIBBHHH', title, version, kind, 0, ext, len(records), 0)
    put(result, 0x20, 'QI', APP, required)
    for name, data, content_type in records:
        result += hashlib.sha256(data).digest() + bytes.fromhex(name[:32])
        result += len(data).to_bytes(6, 'little') + bytes((content_type, 0))
    # Retail CNMT may retain a development-build digest: treat it as opaque.
    result += bytes(range(32))
    pfs = partition([('content.cnmt', result)], False)
    return nca(1, title, [(pfs_header(len(pfs)), pfs)])


def main(directory):
    directory.mkdir(parents=True, exist_ok=True)
    npdm = bytearray(0x80)
    npdm[:4] = b'META'
    npdm[0xc], npdm[0xe] = 1, 44
    put(npdm, 0x1c, 'I', 0x4000)
    npdm[0x20:0x27] = b'Fixture'
    nso = bytearray(0x100)
    nso[:4] = b'NSO0'
    code = partition([('main.npdm', npdm), ('main', nso)], False)
    rom = bytearray((i*19) % 256 for i in range(0x200))
    rom[:0x100] = bytes(0x100)
    put(rom, 0, 'Q', 0x50)
    put(rom, 0x18, 'QQ', 0x50, 0x18)
    put(rom, 0x38, 'QQQ', 0x68, 0x2c, 0x100)
    put(rom, 0x50, 'IIIIII', 0xffffffff, 0xffffffff, 0xffffffff, 0, 0xffffffff, 0)
    put(rom, 0x68, 'IIQQII', 0, 0xffffffff, 0, 0x20, 0xffffffff, 11)
    rom[0x88:0x93] = b'payload.bin'
    base = nca(0, APP, [(pfs_header(len(code)), code), (rom_header(len(rom)), rom)])
    (directory/'content-base.nsp').write_bytes(partition([('base.nca', base)], False))
    (directory/'content-base.nca').write_bytes(base)
    # Two crypto generations, three relocations, and a larger virtual section.
    patch_data = bytearray((i*37+9) % 256 for i in range(0x4000))
    patch_data[0x100:0x10c] = b'PATCHED DATA'
    relocs = table([(0, 0, 0), (0x100, 0x100, 1), (0x400, 0x80, 0)], 0x580, True)
    subsections = table([(0, 0, 10), (0x200, 0, 20)], 0xc000, False)
    fs = rom_header(0x580)
    fs[4] = 4
    put(fs, 0x100, 'QQIIII', 0x4000, len(relocs), 0x52544b42, 1, 3, 0)
    put(fs, 0x120, 'QQIIII', 0xc000, len(subsections), 0x52544b42, 1, 2, 0)
    put(fs, 0x140, 'II', 7, 11)
    updated_code = bytearray(code)
    updated_code[updated_code.find(b'Fixture'):updated_code.find(b'Fixture')+7] = b'Updated'
    patch = nca(0, APP, [(pfs_header(len(code)), updated_code), (fs, patch_data+relocs+subsections)])
    patch[0x206] = 1
    patch[0x300:0x340] = encrypt(AREA_KEY, modes.ECB(), SECTION_KEYS)
    stored = bytearray(patch)
    start = struct.unpack_from('<I', patch, 0x250)[0]*512
    # Code is plaintext; data uses CTREx, tables use ordinary CTR.
    for lo, hi, generation in [(0, 0x200, 10), (0x200, 0x4000, 20), (0x4000, 0x14000, 7)]:
        counter = (11).to_bytes(4, 'big') + generation.to_bytes(4, 'big') + ((start+lo)//16).to_bytes(8, 'big')
        stored[start+lo:start+hi] = encrypt(SECTION_KEYS[32:48], modes.CTR(counter), patch[start+lo:start+hi])
    for i in range(6):
        stored[i*512:(i+1)*512] = encrypt(HEADER_KEY, modes.XTS(i.to_bytes(16, 'big')), patch[i*512:(i+1)*512])
    (directory/'content-patch.nca').write_bytes(stored)
    (directory/'content-patch-expected.nca').write_bytes(patch)
    virtual = rom[:0x100] + patch_data[0x100:0x400] + rom[0x80:0x200]
    (directory/'content-romfs-expected.bin').write_bytes(virtual)
    # Installation fixtures stay plaintext; the standalone NCA above tests crypto.
    stored = patch
    patch_name = '11'*16+'.nca'
    legal_name = '44'*16+'.nca'
    legal = nca(3, APP, [(rom_header(len(rom)), rom)])
    meta = packaged_meta(0x81, APP+0x800, 65536, [(patch_name, stored, 1), (legal_name, legal, 5)])
    update = partition([(patch_name, stored), ('update.cnmt.nca', meta), (legal_name, legal)], False)
    (directory/'content-update.nsp').write_bytes(update)
    # Same payload, newer version: selection must use CNMT, not import order.
    meta2 = packaged_meta(0x81, APP+0x800, 131072, [(patch_name, stored, 1)])
    (directory/'content-update-new.nsp').write_bytes(partition([(patch_name, stored), ('update.cnmt.nca', meta2)], False))
    (directory/'content-bundled.xci').write_bytes(cartridge([('base.nca', base), (patch_name, stored), ('update.cnmt.nca', meta), (legal_name, legal)]))
    # Full replacement Program without BKTR: CNMT must prevent base selection.
    full_name = '33'*16+'.nca'
    full_meta = packaged_meta(0x81, APP+0x800, 196608, [(full_name, base, 1)])
    (directory/'content-full-bundled.xci').write_bytes(cartridge([('base.nca', base), (full_name, base), ('update.cnmt.nca', full_meta)]))
    dlc_rom = bytearray(rom)
    dlc_rom[0x100:0x108] = b'DLC DATA'
    dlc = nca(4, DLC, [(rom_header(len(dlc_rom)), dlc_rom)])
    dlc_name = '22'*16+'.nca'
    dlc_meta = packaged_meta(0x82, DLC, 0, [(dlc_name, dlc, 2)], 65536)
    (directory/'content-dlc.nsp').write_bytes(partition([(dlc_name, dlc), ('dlc.cnmt.nca', dlc_meta)], False))
    (directory/'content-dlc-expected.bin').write_bytes(dlc_rom)
    # Failures must never publish an installation.
    corrupt = bytearray(stored)
    corrupt[-1] ^= 1
    (directory/'content-bad-hash.nsp').write_bytes(partition([(patch_name, corrupt), ('update.cnmt.nca', meta), (legal_name, legal)], False))
    (directory/'content-missing.nsp').write_bytes(partition([('update.cnmt.nca', meta), (legal_name, legal)], False))
    bad_tree = bytearray(patch)
    # Plain header means plaintext data. Damage leaf ordering independently.
    put(bad_tree, start+0x4000+0x4010, 'Q', 1)
    bad_meta = packaged_meta(0x81, APP+0x800, 65536, [(patch_name, bad_tree, 1)])
    (directory/'content-bad-tree.nsp').write_bytes(partition([(patch_name, bad_tree), ('update.cnmt.nca', bad_meta)], False))
    bad_cnmt = bytearray(meta)
    cnmt_offset = bad_cnmt.find((APP+0x800).to_bytes(8, 'little'), 0xc00)
    bad_cnmt[cnmt_offset+0xc] = 0x83
    (directory/'content-bad-cnmt.nsp').write_bytes(partition([(patch_name, stored), ('update.cnmt.nca', bad_cnmt)], False))
    out_of_range = bytearray(patch)
    put(out_of_range, start+0x4000+0x4010+20+8, 'Q', 0xfffffffffffffff0)
    bad_meta = packaged_meta(0x81, APP+0x800, 65536, [(patch_name, out_of_range, 1)])
    (directory/'content-bad-range.nsp').write_bytes(partition([(patch_name, out_of_range), ('update.cnmt.nca', bad_meta)], False))
    multi = bytearray(0xc000)
    put(multi, 0, 'IIQQQ', 0, 2, 0x200, 0, 0x100)
    for b in range(2):
        put(multi, (b+1)*0x4000, 'IIQ', 0, 2, (b+1)*0x100)
        for j in range(2):
            offset = b*0x100+j*0x80
            put(multi, (b+1)*0x4000+16+j*20, 'QQI', offset, offset, j)
    (directory/'content-multi-bucket.bin').write_bytes(multi)
    (directory/'prod.keys').write_text('header_key = '+HEADER_KEY.hex()+'\nkey_area_key_application_00 = '+AREA_KEY.hex()+'\n')
    print('Generated update/DLC/BKTR fixtures in', directory)

if __name__ == '__main__':
    main(pathlib.Path(sys.argv[1]))
