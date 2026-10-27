#!/usr/bin/env python3
"""Checks the MSAA / alpha-to-coverage register constants used by
ThreeD::noteUnimplementedMultisample() (src/.../engines/3d.cpp) against NVIDIA's
clb197.h. Usage: msaa_register_audit.py <clb197.h>"""
import re
import sys
from pathlib import Path

header = Path(sys.argv[1]).read_text()
source = (Path(__file__).resolve().parent.parent /
          'src/core/hw/tegra_x1/gpu/engines/3d.cpp').read_text()


def define(name):
    m = re.search(r'^#define\s+NVB197_%s\s+(\S+)\s*$' % name, header, re.M)
    if not m:
        raise SystemExit('missing in header: ' + name)
    return m[1]


checks = {
    'SET_ANTI_ALIAS': ('0x15d0', 'REG_ANTI_ALIAS = 0x15d0 / 4'),
    'SET_ANTI_ALIAS_ALPHA_CONTROL': ('0x153c', 'REG_ANTI_ALIAS_ALPHA_CONTROL = 0x153c / 4'),
}
for name, (offset, needle) in checks.items():
    if define(name).lower() != offset:
        raise SystemExit('header offset differs for ' + name)
    if needle not in source:
        raise SystemExit('3d.cpp does not use ' + needle)
assert define('SET_ANTI_ALIAS_SAMPLES') == '3:0'
assert define('SET_ANTI_ALIAS_ALPHA_CONTROL_ALPHA_TO_COVERAGE') == '0:0'
assert define('SET_ANTI_ALIAS_ALPHA_CONTROL_ALPHA_TO_ONE') == '4:4'
assert 'alpha_control & 0x1' in source and 'alpha_control & 0x10' in source
assert 'regs_raw[REG_ANTI_ALIAS] & 0xf' in source
print('MSAA register constants match clb197.h (offsets and bit fields)')
