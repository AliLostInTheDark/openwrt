#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Convert Realtek vendor parameter files (PHY_REG.txt, RadioA.txt,
# TXPWR_LMT.txt, ...) into rtw89 firmware elements and append them to an
# rtw89 firmware file.
#
# usage: rtw89-elements.py -o OUT -f FW -r RFE [--bb F]... [--gain F]...
#        [--radio-a F]... [--radio-b F]... [--nctl F] [--byrate F] [--lmt F]
#        [--lmt-ru F] [--trk F] [--bb-ovl F] [--gain-ovl F]
#        [--radio-a-ovl F] [--radio-b-ovl F]
#
# Register tables given more than once are concatenated in order, which
# matches the vendor driver applying a front-end module overlay on top of
# the generic RFE tables. The --*-ovl options do the same for a table that
# FW already carries: the overlay is appended to that element in place, as
# rtw89 uses the first element of each table and ignores later ones.

import argparse
import re
import struct

ELM_BB_REG = 2
ELM_BB_GAIN = 3
ELM_RADIO_A = 4
ELM_RADIO_B = 5
ELM_RF_NCTL = 8
ELM_TXPWR_BYRATE = 9
ELM_TXPWR_LMT_2GHZ = 10
ELM_TXPWR_LMT_5GHZ = 11
ELM_TXPWR_LMT_RU_2GHZ = 13
ELM_TXPWR_LMT_RU_5GHZ = 14
ELM_TX_SHAPE_LMT = 16
ELM_TXPWR_TRK = 18

ELEMENT_ALIGN = 16

RS_CCK, RS_OFDM, RS_MCS, RS_HEDCM = 0, 1, 2, 3

REGD = {
    'WW': 0, 'ETSI': 1, 'FCC': 2, 'MKK': 3, 'IC': 5, 'KCC': 6, 'ACMA': 7,
    'NCC': 8, 'MEXICO': 9, 'CHILE': 10, 'UKRAINE': 11, 'CN': 12,
    'QATAR': 13, 'UK': 14, 'THAILAND': 15,
}
LMT_NA = 127


def strip(line):
    return line.split('//')[0].strip()


def ch_idx(ch):
    if 1 <= ch <= 14:
        return ch - 1
    if 36 <= ch <= 64:
        return (ch - 36) // 2
    if 100 <= ch <= 144:
        return (ch - 100) // 2 + 15
    if 149 <= ch <= 177:
        return (ch - 149) // 2 + 38
    raise ValueError('channel %d' % ch)


# Version of the tables this script generates, in the byte Realtek uses for
# its own elements ("00 28 00 00" = 40). Bump it when the generated tables
# change; rtw89 only prints it.
ELM_VER = bytes((0, 1, 0, 0))


def element(elm_id, fixed, payload, ver=ELM_VER):
    hdr = struct.pack('<II4sHHII', elm_id, len(payload), ver, 0, 0, 0, 0)
    return hdr + fixed + payload


def read_regs(files, gain=False):
    regs = b''
    for f in files:
        for line in open(f, encoding='latin-1'):
            m = re.match(r'(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)', strip(line))
            if not m:
                continue
            addr, data = int(m.group(1), 16), int(m.group(2), 16)
            if addr == 0xffff and data == 0xffff:
                break  # end of table
            # the vendor driver ignores gain error entries past sub-type 2
            if gain and addr >> 24 == 0 and addr & 0xff > 2:
                continue
            regs += struct.pack('<II', addr, data)
    return regs


def reg_element(elm_id, files, idx=0):
    regs = read_regs(files, elm_id == ELM_BB_GAIN)
    return element(elm_id, struct.pack('<B7x', idx), regs)


def apply_overlays(fw, ovl):
    """Append overlay registers to the matching register elements of fw."""
    nr = fw[1]
    last = 16 + (nr - 1) * 16
    shift, size = struct.unpack_from('<II', fw, last + 4)
    off = shift + size
    out = fw[:off]
    off += -off % ELEMENT_ALIGN
    while off + 32 <= len(fw):
        elm_id, size = struct.unpack_from('<II', fw, off)
        e = bytearray(fw[off:off + 32 + size])
        if elm_id in ovl:
            e += read_regs(ovl.pop(elm_id), elm_id == ELM_BB_GAIN)
            struct.pack_into('<I', e, 4, len(e) - 32)
        out += b'\0' * (-len(out) % ELEMENT_ALIGN) + e
        off += 32 + size
        off += -off % ELEMENT_ALIGN
    if ovl:
        raise SystemExit('no element for overlay %s' % list(ovl))
    return out


def txpwr_element(elm_id, rfe, ent_sz, ents):
    fixed = struct.pack('<BBBBI', 0, 0, rfe, ent_sz, len(ents))
    return element(elm_id, fixed, b''.join(ents))


BYRATE_RS = {
    '11M_1M': (RS_CCK, 0), '18M_6M': (RS_OFDM, 0), '54M_24M': (RS_OFDM, 4),
    'MCS3_0': (RS_MCS, 0), 'MCS7_4': (RS_MCS, 4), 'MCS11_8': (RS_MCS, 8),
    'DCM4_0': (RS_HEDCM, 0),
}
BAND = {'2.4G': 0, '5G': 1, '6G': 2}


def byrate(f, rfe):
    ents = []
    band = None
    for line in open(f, encoding='latin-1'):
        line = strip(line)
        m = re.match(r'#\[(2\.4G|5G|6G)\]#', line)
        if m:
            band = BAND[m.group(1)]
            continue
        m = re.match(r'\[Nss(\d)\]\s+(\S+)\s+(.*)', line)
        if not m or m.group(2) not in BYRATE_RS:
            continue
        rs, shf = BYRATE_RS[m.group(2)]
        vals = [round(float(v) * 4) for v in m.group(3).split()]
        data = 0
        for v in vals:  # listed from the highest rate to the lowest
            data = (data << 8) | (v & 0xff)
        ents.append(struct.pack('<BBBBBIBB', band, int(m.group(1)) - 1, rs,
                                shf, len(vals), data, 0, 0))
    return txpwr_element(ELM_TXPWR_BYRATE, rfe, 11, ents)


def lmt_tables(f):
    """Yield (header fields, {regd: {ch: value}}) for each table in f."""
    head = cols = None
    rows = {}
    for line in open(f, encoding='latin-1'):
        s = line.strip()
        m = re.match(r'##\s+(2\.4G|5G|6G),\s*(.*?)\s*(//.*)?$', s)
        if m:
            head = [m.group(1)] + [x.strip() for x in m.group(2).split(',')]
            cols = None
            rows = {}
            continue
        if head is None:
            continue
        if s.startswith('##'):
            words = s[2:].split()
            if words and words[0] == 'END':
                yield head, rows
                head = None
            elif words and words[0] in REGD:
                cols = words
            continue
        words = strip(line).split()
        if cols and words and re.match(r'CH\d+$', words[0]):
            ch = int(words[0][2:])
            for regd, v in zip(cols, words[1:]):
                rows.setdefault(regd, {})[ch] = \
                    LMT_NA if v == 'NA' else round(float(v) * 4)


def with_ww(rows):
    """Add the WW column: the lowest limit of all regulations."""
    ww = {}
    for col in rows.values():
        for ch, v in col.items():
            if v != LMT_NA:
                ww[ch] = min(ww.get(ch, LMT_NA), v)
            else:
                ww.setdefault(ch, LMT_NA)
    rows = dict(rows)
    rows['WW'] = ww
    return rows


BW = {'20M': 0, '40M': 1, '80M': 2, '160M': 3}
LMT_RS = {'CCK': RS_CCK, 'OFDM': RS_OFDM, 'HT': RS_MCS, 'HE': RS_MCS}
RU = {'RU26': 0, 'RU52': 1, 'RU106': 2}


def lmt(f, rfe):
    ents = {0: [], 1: []}
    for (band, bw, nt, rs, bf), rows in lmt_tables(f):
        if band == '6G':
            continue
        b = BAND[band]
        for regd, col in with_ww(rows).items():
            for ch, v in sorted(col.items()):
                ents[b].append(struct.pack('<BBBBBBb', BW[bw], int(nt[0]) - 1,
                                           LMT_RS[rs], bf == 'BF',
                                           REGD[regd], ch_idx(ch), v))
    return [txpwr_element(ELM_TXPWR_LMT_2GHZ, rfe, 7, ents[0]),
            txpwr_element(ELM_TXPWR_LMT_5GHZ, rfe, 7, ents[1])]


def tx_shape(f, rfe):
    """TX shape index per band, CCK/OFDM and regulation (PShape rows)."""
    shapes = {}
    head = cols = None
    for line in open(f, encoding='latin-1'):
        s = line.strip()
        m = re.match(r'##\s+(2\.4G|5G|6G),\s*(.*?)\s*(//.*)?$', s)
        if m:
            head = [m.group(1)] + [x.strip() for x in m.group(2).split(',')]
            cols = None
            continue
        if head is None:
            continue
        if s.startswith('##'):
            words = s[2:].split()
            if words and words[0] in REGD:
                cols = words
            continue
        words = strip(line).split()
        if cols and words and words[0] == 'PShape' and head[0] != '6G':
            rs = RS_CCK if head[3] == 'CCK' else RS_OFDM
            for regd, v in zip(cols, words[1:]):
                shapes[(BAND[head[0]], rs, REGD[regd])] = int(v)
    for b, rs in {(b, rs) for b, rs, _ in shapes}:
        vals = [v for (b2, rs2, _), v in shapes.items() if (b2, rs2) == (b, rs)]
        shapes.setdefault((b, rs, REGD['WW']), min(vals))
    ents = [struct.pack('<BBBB', b, rs, regd, v)
            for (b, rs, regd), v in sorted(shapes.items())]
    return txpwr_element(ELM_TX_SHAPE_LMT, rfe, 4, ents)


def lmt_ru(f, rfe):
    ents = {0: [], 1: []}
    for (band, ru, nt, *_), rows in lmt_tables(f):
        if band == '6G':
            continue
        b = BAND[band]
        for regd, col in with_ww(rows).items():
            for ch, v in sorted(col.items()):
                ents[b].append(struct.pack('<BBBBb', RU[ru], int(nt[0]) - 1,
                                           REGD[regd], ch_idx(ch), v))
    return [txpwr_element(ELM_TXPWR_LMT_RU_2GHZ, rfe, 5, ents[0]),
            txpwr_element(ELM_TXPWR_LMT_RU_5GHZ, rfe, 5, ents[1])]


# enum rtw89_fw_txpwr_trk_type, with the number of sub-bands of each
TRK_TYPES = [
    ('6G', 'B', '-', 'ALL', 4), ('6G', 'B', '+', 'ALL', 4),
    ('6G', 'A', '-', 'ALL', 4), ('6G', 'A', '+', 'ALL', 4),
    ('5G', 'B', '-', 'ALL', 3), ('5G', 'B', '+', 'ALL', 3),
    ('5G', 'A', '-', 'ALL', 3), ('5G', 'A', '+', 'ALL', 3),
    ('2G', 'B', '-', 'ALL', 1), ('2G', 'B', '+', 'ALL', 1),
    ('2G', 'A', '-', 'ALL', 1), ('2G', 'A', '+', 'ALL', 1),
    ('2G', 'B', '-', 'CCK', 1), ('2G', 'B', '+', 'CCK', 1),
    ('2G', 'A', '-', 'CCK', 1), ('2G', 'A', '+', 'CCK', 1),
]
SWINGIDX = 30


def trk(f):
    tbl = {}
    for line in open(f, encoding='latin-1'):
        m = re.match(r'((?:\[[^\]]*\])+)\s*=\s*\{([^}]*)\}', strip(line))
        if m:
            key = tuple(re.findall(r'\[([^\]]*)\]', m.group(1)))
            tbl[key] = [int(v) for v in m.group(2).split(',')]
    bitmap = 0
    payload = b''
    for t, (band, path, sign, rs, nsub) in enumerate(TRK_TYPES):
        subs = [(band, path, sign, rs) if nsub == 1 else
                (band, path, sign, rs, str(i)) for i in range(nsub)]
        if not all(k in tbl for k in subs):
            continue
        bitmap |= 1 << t
        for k in subs:
            vals = tbl[k]
            assert len(vals) == SWINGIDX, (k, len(vals))
            payload += struct.pack('<30b', *vals)
    return element(ELM_TXPWR_TRK, struct.pack('<II', bitmap, 0), payload)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('-o', required=True)
    p.add_argument('-f', required=True)
    p.add_argument('-r', type=int, required=True)
    for a in ('bb', 'gain', 'radio-a', 'radio-b'):
        p.add_argument('--' + a, action='append', default=[])
    for a in ('nctl', 'byrate', 'lmt', 'lmt-ru', 'trk'):
        p.add_argument('--' + a)
    p.add_argument('--tx-shape', action='store_true')
    for a in ('bb-ovl', 'gain-ovl', 'radio-a-ovl', 'radio-b-ovl'):
        p.add_argument('--' + a, action='append', default=[])
    a = p.parse_args()

    elms = []
    if a.bb:
        elms.append(reg_element(ELM_BB_REG, a.bb))
    if a.gain:
        elms.append(reg_element(ELM_BB_GAIN, a.gain))
    if a.radio_a:
        elms.append(reg_element(ELM_RADIO_A, a.radio_a, 0))
    if a.radio_b:
        elms.append(reg_element(ELM_RADIO_B, a.radio_b, 1))
    if a.nctl:
        elms.append(reg_element(ELM_RF_NCTL, [a.nctl]))
    if a.byrate:
        elms.append(byrate(a.byrate, a.r))
    if a.lmt:
        elms += lmt(a.lmt, a.r)
        if a.tx_shape:
            elms.append(tx_shape(a.lmt, a.r))
    if a.lmt_ru:
        elms += lmt_ru(a.lmt_ru, a.r)
    if a.trk:
        elms.append(trk(a.trk))

    fw = open(a.f, 'rb').read()
    ovl = {k: v for k, v in ((ELM_BB_REG, a.bb_ovl),
                             (ELM_BB_GAIN, a.gain_ovl),
                             (ELM_RADIO_A, a.radio_a_ovl),
                             (ELM_RADIO_B, a.radio_b_ovl)) if v}
    if ovl:
        fw = apply_overlays(fw, ovl)
    fw += b'\0' * (-len(fw) % ELEMENT_ALIGN)
    for e in elms:
        fw += e + b'\0' * (-len(e) % ELEMENT_ALIGN)
    open(a.o, 'wb').write(fw)


if __name__ == '__main__':
    main()
