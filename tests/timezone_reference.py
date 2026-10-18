#!/usr/bin/env python3
"""POSIX TZ footer extension of Hydra's TZif parser vs Python's zoneinfo (tzdata).

Builds Nintendo-style blobs (one TZif block with 64-bit transitions, then the
"\\nPOSIX-TZ\\n" footer) from the system tzdata, deliberately dropping every stored
transition after 2020 so later local times depend entirely on the footer rule.
Hydra's parser expands the rule; the lookup below follows tzcode's localsub(),
and offset, DST flag and abbreviation are compared with zoneinfo for 2021-2100
(plus 2030s/2090s edge hours around each transition).

Usage: timezone_reference.py <hydra-timezone-tests>
Limit: zones whose rule is unchanged from 2021 on (checked against tzdata); the
tzdata in use must be current. No Nintendo firmware data is involved.
"""
import bisect
import datetime as dt
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from zoneinfo import ZoneInfo, TZPATH

CUTOFF = int(dt.datetime(2020, 12, 31, tzinfo=dt.timezone.utc).timestamp())
ZONES = ['America/New_York', 'America/Los_Angeles', 'America/Sao_Paulo', 'Europe/London',
         'Europe/Paris', 'Europe/Dublin', 'Australia/Sydney', 'Australia/Lord_Howe',
         'Pacific/Auckland', 'Asia/Tokyo', 'Asia/Kolkata', 'Asia/Kathmandu', 'America/Santiago',
         'America/St_Johns', 'UTC']


# Zones whose recent years deviate from the footer rule (one-off government
# changes kept as stored transitions): compare only from this year on.
COMPARE_FROM = {'America/Santiago': 2025}


def read_tzif(path):
    data = Path(path).read_bytes()
    assert data[:4] == b'TZif'
    version = data[4]
    counts = struct.unpack('>6l', data[20:44])
    isutc, isstd, leap, timecnt, typecnt, charcnt = counts
    pos = 44
    block1 = timecnt * 4 + timecnt + typecnt * 6 + charcnt + leap * 8 + isstd + isutc
    pos += block1
    if version == 0:
        raise SystemExit('v1 only file: ' + path)
    counts = struct.unpack('>6l', data[pos + 20:pos + 44])
    isutc, isstd, leap, timecnt, typecnt, charcnt = counts
    pos += 44
    ats = list(struct.unpack('>%dq' % timecnt, data[pos:pos + 8 * timecnt]))
    pos += 8 * timecnt
    idx = list(data[pos:pos + timecnt])
    pos += timecnt
    types = [struct.unpack('>lBB', data[pos + 6 * i:pos + 6 * i + 6]) for i in range(typecnt)]
    pos += 6 * typecnt
    chars = data[pos:pos + charcnt]
    pos += charcnt + leap * 12 + isstd + isutc
    footer = data[pos:].strip(b'\n').decode()
    return ats, idx, types, chars, footer


def build_blob(zone):
    path = next(Path(p) / zone for p in TZPATH if (Path(p) / zone).exists())
    ats, idx, types, chars, footer = read_tzif(path)
    keep = [i for i, a in enumerate(ats) if a <= CUTOFF]
    ats, idx = [ats[i] for i in keep], [idx[i] for i in keep]
    if not ats:  # parser needs a type table only; an empty list is legal
        pass
    body = b''.join(struct.pack('>q', a) for a in ats) + bytes(idx)
    body += b''.join(struct.pack('>lBB', off, dst, ab) for off, dst, ab in types)
    body += chars
    body += bytes([0]) * 0  # no leap seconds
    header = b'TZif' + b'2' + bytes(15) + struct.pack('>6l', 0, 0, 0, len(ats), len(types), len(chars))
    return header + body + b'\n' + footer.encode() + b'\n', footer, (ats, idx, types, chars)


def parse_dump(lines):
    time_count, type_count, char_count, default_type, go_back, go_ahead = map(int, lines[0].split())
    ats = list(map(int, lines[1].split()))
    idx = list(map(int, lines[2].split()))
    infos = [tuple(map(int, t.split(','))) for t in lines[3].split()]
    chars = bytes.fromhex(lines[4])
    return dict(ats=ats, idx=idx, infos=infos, chars=chars, default_type=default_type)


def local_info(rule, t):
    """tzcode localsub(): type in effect at UTC time t."""
    ats = rule['ats']
    if not ats or t < ats[0]:
        i = rule['default_type']
    else:
        i = rule['idx'][bisect.bisect_right(ats, t) - 1]
    off, dst, ab = rule['infos'][i]
    name = rule['chars'][ab:rule['chars'].index(b'\0', ab)].decode()
    return off, bool(dst), name


def main():
    binary = sys.argv[1]
    blobs, footers = [], []
    for zone in ZONES:
        blob, footer, _ = build_blob(zone)
        blobs.append(blob)
        footers.append(footer)
    with tempfile.TemporaryDirectory() as tmp:
        src, out = Path(tmp) / 'blobs.txt', Path(tmp) / 'out.txt'
        src.write_text('\n'.join(b.hex() for b in blobs) + '\n')
        completed = subprocess.run([binary, str(src), str(out)])
        if completed.returncode != 0:
            return completed.returncode
        lines = out.read_text().splitlines()
    failures = 0
    for n, zone in enumerate(ZONES):
        rule = parse_dump(lines[5 * n:5 * n + 5])
        tz = ZoneInfo(zone)
        # Probe times: every 6 hours-ish around the year plus the hours around each transition
        probes = []
        t = int(dt.datetime(2021, 1, 1, tzinfo=dt.timezone.utc).timestamp())
        end = int(dt.datetime(2100, 1, 1, tzinfo=dt.timezone.utc).timestamp())
        while t < end:
            probes.append(t)
            t += 6 * 3600 + 17 * 60
        for a in rule['ats']:
            if a > CUTOFF:
                probes += [a - 3601, a - 1, a, a + 1, a + 3601]
        bad = 0
        for t in probes:
            first = int(dt.datetime(COMPARE_FROM.get(zone, 2021), 1, 1, tzinfo=dt.timezone.utc).timestamp())
            if t < max(CUTOFF + 86400, first) or t >= end + 86400 * 400:
                continue
            exp = dt.datetime.fromtimestamp(t, tz)
            exp_off = int(exp.utcoffset().total_seconds())
            exp_dst = bool(exp.dst())
            got = local_info(rule, t)
            if (got[0], got[1], got[2]) != (exp_off, exp_dst, exp.tzname()):
                bad += 1
                if bad <= 3:
                    print('   %s t=%d expected=(%d,%s,%s) got=%s' % (zone, t, exp_off, exp_dst, exp.tzname(), got))
        status = 'ok ' if bad == 0 else 'BAD'
        print('%s %-22s footer=%-38s transitions=%-4d probes=%d bad=%d' % (
            status, zone, footers[n], len(rule['ats']), len(probes), bad))
        failures += bad != 0
    print('%d zones with mismatches' % failures)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
