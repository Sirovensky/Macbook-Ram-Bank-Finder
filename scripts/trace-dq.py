#!/usr/bin/env python3
"""
Derive per-chip (channel, rank, byte_lane) mapping from a two-stage trace:

  CPU-side (pdftotext of schematic):
    MEM_<A|B>_DQ<NN>   <cpu_bga_pin>   DDR<0|1>_DQ<M>
  Chip-side (same schematic, per-chip pin block):
    <chip_bga_pin>   <U_designator>   DQ<k>
    MEM_<A|B>_DQ<NN>                       (the net on that pin)

Join: chip pin DQ<k> -> MEM_X_DQ<NN> -> DDRc_DQ<M>.
Byte lane = M / 8. Chip width inferred from #distinct lanes per chip
(should be 1 for x8, 2 for x16).

Input: a `pdftotext -layout` dump of the schematic PDF.

Usage:
  scripts/trace-dq.py vendor/820-01814-full.txt

Prints a YAML-ready table of verified mappings. Does NOT overwrite the
topology file — copy/paste or pipe. Ranks cannot be derived from this
alone (CS# nets must be traced separately); emits rank=0 for all chips
on boards where only one CS# is routed per channel.
"""

import re, sys, argparse
from collections import Counter


def load_cpu_map(txt):
    m = {}
    for g in re.finditer(r'MEM_([AB])_DQ<(\d+)>\s+\S+\s+DDR([01])_DQ(\d+)', txt):
        m[(g.group(1), int(g.group(2)))] = (int(g.group(3)), int(g.group(4)))
    return m


def chip_pin_trace(txt, u_pat=r'U(2[3-6][0-3]0)'):
    """Return: {U -> [(chip_dq_num, mem_side, mem_net_num)]}"""
    lines = txt.split('\n')
    u_re = re.compile(u_pat)
    dq_re = re.compile(r'\bDQ(\d+)(?:/NC)?\b')
    net_re = re.compile(r'MEM_([AB])_DQ<(\d+)>')
    anchors = [(i, m.start(), 'U' + m.group(1))
               for i, ln in enumerate(lines) for m in u_re.finditer(ln)]

    out = {}
    for li, col, u in anchors:
        left, right = max(0, col - 30), col + 250
        band_dqs, band_nets = [], []
        for j in range(li, min(len(lines), li + 80)):
            seg = lines[j][left:right] if len(lines[j]) > left else ''
            for m in dq_re.finditer(seg):
                band_dqs.append((j, m.start(), int(m.group(1))))
            for m in net_re.finditer(seg):
                band_nets.append((j, m.start(), m.group(1), int(m.group(2))))

        rows = []
        for dj, dc, dq in band_dqs:
            best, bestd = None, 9999
            for nj, nc, side, nn in band_nets:
                if nj < dj or nj > dj + 4:
                    continue
                d = (nj - dj) * 10 + abs(nc - dc)
                if d < bestd:
                    best, bestd = (side, nn), d
            if best:
                rows.append((dq, best[0], best[1]))

        seen, uniq = set(), []
        for r in rows:
            if r[0] in seen:
                continue
            seen.add(r[0])
            uniq.append(r)
        out[u] = sorted(uniq)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('txt', help='pdftotext -layout of schematic')
    args = ap.parse_args()
    txt = open(args.txt).read()
    cpu = load_cpu_map(txt)
    chips = chip_pin_trace(txt)

    print(f'# CPU nets mapped: {len(cpu)}')
    print(f'# Chips found: {len(chips)}')
    print('# U-desig   CH  Rank  Lane  Width  Samples(chip_DQ -> intel_bit)')
    for u in sorted(chips):
        rows = chips[u]
        per = []
        lanes = []
        chs = []
        for cdq, side, nn in rows:
            if (side, nn) not in cpu:
                continue
            ch, ib = cpu[(side, nn)]
            per.append((cdq, ib))
            lanes.append(ib // 8)
            chs.append(ch)
        if not per:
            continue
        ch = Counter(chs).most_common(1)[0][0]
        lane_count = len(set(lanes))
        width = 8 if lane_count == 1 else 16 if lane_count == 2 else -1
        lane = Counter(lanes).most_common(1)[0][0]
        print(f'  {u}    CH{ch}  R0    L{lane}   x{width:<2}   {per[:4]}')


if __name__ == '__main__':
    main()
