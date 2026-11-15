#!/usr/bin/env python3
"""
NTSC -> PAL address translation for HP: Chamber of Secrets (the port is written against the NTSC XBE).

The two retail XBEs are separate links of the same source (same title ID 4541000E, same file size, same signature key,
same 13 sections): PAL's .text is 0x260 bytes longer, its functions moved in blocks, every later section starts
0x260 higher, and .data grows inside (initialised part +0xF0, so DOLBY and the rest sit +0x360 higher).

Made from PS's port/tools/edition/pal2ntsc.py (the same method), with the layout read from the two XBEs instead of
held as constants:

`map` matches every NTSC .text function to PAL by its bytes with the address operands masked (rel32 call/jmp/jcc
targets, absolute displacements and immediates inside the image), fills small functions from their matched neighbours'
shift, settles look-alikes by call votes, and reads every operand the matched pairs share into an exact table. Data
after .text is placed by runs of one shift each, taken from those operands (a section's start shift where no operand
says otherwise).

`translate` rewrites sub_/loc_XXXXXXXX names, 0x literals and bare (00XXXXXX macro arguments in the given files; what
it cannot place with certainty is listed under REVIEW and left alone (`--strict` fails on it: decide it in rules.txt).

    python3 tools/edition/ntsc2pal.py map NTSC.xbe PAL.xbe ntsc_functions.json -o ntsc2pal_map.json
    python3 tools/edition/ntsc2pal.py translate ntsc2pal_map.json FILE... [--dry-run]
"""
import argparse, bisect, collections, json, os, re, struct, sys


def load_xbe(path):
    d = open(path, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    ns = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = []
    for i in range(ns):
        _fl, va, vs, ra, rs, na = struct.unpack_from('<IIIIII', d, sh + i * 56)
        name = d[na - base:d.index(b'\0', na - base)].decode()
        secs.append((name, va, vs, d[ra:ra + rs]))
    return secs


def text_of(secs):
    return next(s for s in secs if s[0] == '.text')


class Masker:
    def __init__(self, text_va, text, image_end):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.va, self.text, self.image_end = text_va, text, image_end
        self.cache = {}

    def masked(self, start, size):
        k = (start, size)
        if k in self.cache:
            return self.cache[k]
        code = self.text[start - self.va:start - self.va + size]
        mask = bytearray(b'\x01' * len(code))
        starts = set()
        for ins in self.md.disasm(code, start):
            o = ins.address - start
            starts.add(ins.address)
            b0 = ins.bytes[0]
            if b0 in (0xE8, 0xE9) and ins.size == 5 or b0 == 0x0F and ins.size == 6 and 0x80 <= ins.bytes[1] <= 0x8F:
                mask[o + ins.size - 4:o + ins.size] = b'\0\0\0\0'
                continue
            for off, sz in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
                if sz == 4:
                    v = struct.unpack_from('<I', code, o + off)[0]
                    if 0x10000 <= v < self.image_end + 0x1000:
                        mask[o + off:o + off + 4] = b'\0\0\0\0'
        self.cache[k] = (code, mask, starts)
        return self.cache[k]


def build_map(src_xbe, dst_xbe, funcs_json):
    S, D = load_xbe(src_xbe), load_xbe(dst_xbe)
    if [s[0] for s in S] != [s[0] for s in D]:
        sys.exit('the two XBEs have different sections')
    _, TEXT, src_text_size, P = text_of(S)
    _, dtext, _, N = text_of(D)
    assert TEXT == dtext
    src_text_end = TEXT + src_text_size
    image_end = max(va + vs for _, va, vs, _ in S)
    mk = Masker(TEXT, P, image_end)
    funcs = sorted((f for f in json.load(open(funcs_json)) if f['section'] == '.text'), key=lambda f: int(f['start'], 16))
    K = 12
    idx = collections.defaultdict(list)
    for i in range(len(N) - K):
        idx[N[i:i + K]].append(i)

    def fits(code, mask, b):
        o = b - TEXT
        if o < 0 or o + len(code) > len(N):
            return False
        return all(m == 0 or N[o + k] == code[k] for k, m in enumerate(mask))

    m = {}
    for f in funcs:
        a = int(f['start'], 16)
        code, mask, _ = mk.masked(a, f['size'])
        key = next((i for i in range(max(0, len(code) - K)) if all(mask[i:i + K])), None)
        if key is None:
            continue
        cands = [j - key + TEXT for j in idx.get(bytes(code[key:key + K]), []) if fits(code, mask, j - key + TEXT)]
        if cands:
            m[a] = min(cands, key=lambda c: abs(c - a))
    for _ in range(3):  # neighbours' shift for short functions and to settle ambiguous ones
        for i, f in enumerate(funcs):
            a = int(f['start'], 16)
            deltas = []
            for d in (-1, 1, -2, 2):
                j = i + d
                if 0 <= j < len(funcs) and int(funcs[j]['start'], 16) in m:
                    aj = int(funcs[j]['start'], 16)
                    deltas.append(m[aj] - aj)
            if a in m and m[a] - a in deltas[:2]:
                continue
            code, mask, _ = mk.masked(a, f['size'])
            for dl in deltas:
                if fits(code, mask, a + dl):
                    m[a] = a + dl
                    break

    def operands():
        for f in funcs:
            a = int(f['start'], 16)
            n = m.get(a)
            if n is None:
                continue
            code, mask, _ = mk.masked(a, f['size'])
            if not fits(code, mask, n):  # placed by votes alone: its bytes differ, so its operands don't pair up
                continue
            k = 0
            while k < len(code):
                if mask[k] == 0:
                    pv = struct.unpack_from('<I', code, k)[0]
                    nv = struct.unpack_from('<I', N, n - TEXT + k)[0]
                    rel = k >= 1 and (code[k - 1] in (0xE8, 0xE9) or k >= 2 and code[k - 2] == 0x0F and 0x80 <= code[k - 1] <= 0x8F)
                    yield rel, a + k, n + k, pv, nv
                    k += 4
                else:
                    k += 1
    starts = {int(f['start'], 16) for f in funcs}
    for _ in range(2):
        votes = collections.defaultdict(collections.Counter)
        for rel, pa, na, pv, nv in operands():
            if rel:
                pt, nt = (pa + 4 + pv) & 0xFFFFFFFF, (na + 4 + nv) & 0xFFFFFFFF
                if pt in starts:
                    votes[pt][nt] += 1
        fixed = 0
        for pt, c in votes.items():
            (nt, cnt), = c.most_common(1)
            if len(c) == 1 and m.get(pt) != nt and TEXT <= nt < TEXT + len(N):
                m[pt] = nt
                fixed += 1
        print(f'call votes set {fixed} functions', file=sys.stderr)
    seen = collections.defaultdict(collections.Counter)
    for rel, pa, na, pv, nv in operands():
        if rel:
            pv, nv = (pa + 4 + pv) & 0xFFFFFFFF, (na + 4 + nv) & 0xFFFFFFFF
        if TEXT <= pv < image_end:
            seen[pv][nv] += 1
    exact = {pv: next(iter(c)) for pv, c in seen.items() if len(c) == 1}
    conflicts = sum(1 for c in seen.values() if len(c) > 1)
    moved = 0
    for a in starts:
        nt = exact.get(a)
        if nt is not None and TEXT <= nt < TEXT + len(N) and m.get(a) != nt:
            m[a] = nt
            moved += 1
    print(f'pointers set {moved} functions, {len(exact)} exact operands ({conflicts} with more than one answer)',
          file=sys.stderr)
    # data after .text: each section's start shift, then runs of one shift from the operands the code uses
    sec_shift = [[sva, dva - sva] for (_, sva, _, _), (_, dva, _, _) in zip(S, D) if sva >= src_text_end]
    obs = collections.defaultdict(collections.Counter)
    for rel, pa, na, pv, nv in operands():
        if not rel and src_text_end <= pv < image_end:
            obs[pv][nv - pv] += 1
    runs = []
    for pv in sorted(obs):
        d, cnt = obs[pv].most_common(1)[0]
        if runs and runs[-1][2] == d:
            runs[-1][1] = pv
            runs[-1][3] += cnt
        else:
            runs.append([pv, pv, d, cnt])
    # a lone address with a shift of its own is likely a constant that looks like an address: left to the exact table
    def sec_default(v):
        i = bisect.bisect_right([s[0] for s in sec_shift], v) - 1
        return sec_shift[i][1] if i >= 0 else None
    # and a shift far from every section's (0 for constants such as 0x220000..0x303030 colours) is no data at all
    lo_sh = min(s[1] for s in sec_shift) - 0x40
    hi_sh = max(s[1] for s in sec_shift) + 0x40
    runs = [r for r in runs if lo_sh <= r[2] <= hi_sh and (r[2] == sec_default(r[0]) or r[1] > r[0])]
    merged = []
    for r in runs:
        if merged and merged[-1][2] == r[2] and sec_default(merged[-1][0]) == sec_default(r[0]):
            merged[-1][1] = r[1]
        else:
            merged.append(r[:3])
    # between two runs of different shifts, where the initialised bytes say so: the last address only the first shift
    # matches and the first only the second does (8 bytes compared, image addresses masked) give the boundary
    def raw(secs, va, n):
        for _, sva, _, r in secs:
            if sva <= va and va + n <= sva + len(r):
                return r[va - sva:va - sva + n]
        return None

    def match(v, d):  # exact bytes: a pointer in the window fails both shifts, which decides nothing
        a, b = raw(S, v, 8), raw(D, v + d, 8)
        if a is None or b is None:
            return None
        return a == b
    for r1, r2 in zip(merged, merged[1:]):
        if r1[2] == r2[2] or sec_default(r1[0]) != sec_default(r2[0]) or r2[0] - r1[1] > 0x10000:
            continue
        only1, only2, unknown = [], [], False
        for v in range(r1[1] + 1, r2[0]):
            m1, m2 = match(v, r1[2]), match(v, r2[2])
            if m1 is None or m2 is None:
                unknown = True
                break
            if m1 and not m2:
                only1.append(v)
            elif m2 and not m1:
                only2.append(v)
        if unknown or not only2 or (only1 and max(only1) >= min(only2)):
            continue
        # what lies between (bytes both shifts match, zeros) stays a gap: translate reports it
        e1, b2 = max(only1) if only1 else r1[1], min(only2)
        print(f'data {r1[2]:+#x} up to {e1:#x}, {r2[2]:+#x} from {b2:#x} (the bytes)', file=sys.stderr)
        r1[1], r2[0] = e1, b2
    for r in merged:
        print(f'data {r[0]:#x}-{r[1]:#x} {r[2]:+#x}', file=sys.stderr)
    out = {'text': TEXT, 'src_text_end': src_text_end, 'dst_text_end': TEXT + len(N), 'image_end': image_end,
           'sections': sec_shift,
           'functions': [[int(f['start'], 16), int(f['end'], 16), m.get(int(f['start'], 16))] for f in funcs],
           'data': merged,
           'exact': [[k, v] for k, v in sorted(exact.items())]}
    print(f'matched {sum(1 for f in out["functions"] if f[2])} of {len(funcs)} .text functions', file=sys.stderr)
    return out


class Translator:
    def __init__(self, mp, src_xbe=None, dst_xbe=None):
        self.TEXT, self.text_end, self.image_end = mp['text'], mp['src_text_end'], mp['image_end']
        self.f = mp['functions']
        self.sec = mp['sections']
        self.sec_starts = [s[0] for s in self.sec]
        self.data = mp['data']
        self.dstarts = [d[0] for d in self.data]
        self.exact = dict(map(tuple, mp.get('exact', [])))
        self.starts = [f[0] for f in self.f]
        self.mk = None
        if src_xbe:
            _, va, vs, t = text_of(load_xbe(src_xbe))
            self.mk = Masker(va, t, self.image_end)
        self.mk2 = None
        if src_xbe and dst_xbe:
            _, va, vs, t = text_of(load_xbe(dst_xbe))
            self.mk2 = Masker(va, t, self.image_end + 0x1000)

    def same_prefix(self, s, n, k):
        """the NTSC function at s and its PAL copy at n hold the same instructions (address operands masked) over
        their first k bytes, so an address k bytes into one is k bytes into the other"""
        c1, m1, _ = self.mk.masked(s, k)
        c2, m2, _ = self.mk2.masked(n, k)
        return m1 == m2 and all(a == b for a, b, m in zip(c1, c2, m1) if m)

    def data_shift(self, v):
        i = bisect.bisect_right(self.sec_starts, v) - 1
        sd = self.sec[i][1]
        sec_lo = self.sec[i][0]
        sec_hi = self.sec[i + 1][0] if i + 1 < len(self.sec) else self.image_end
        j = bisect.bisect_right(self.dstarts, v) - 1
        if j >= 0 and v <= self.data[j][1]:
            return self.data[j][2], 'data run'
        before = self.data[j] if j >= 0 and self.data[j][1] >= sec_lo else None
        after = self.data[j + 1] if j + 1 < len(self.data) and self.data[j + 1][0] < sec_hi else None
        lo = before[2] if before else sd
        hi = after[2] if after else lo
        if not before and after:
            lo = hi if hi == sd else sd
        if lo == hi:
            return lo, 'data' if lo == sd else 'data run'
        return None, f'between data runs {lo:+#x} / {hi:+#x}'

    def tr(self, v):
        """-> (new value or None, note)"""
        if v in self.exact:
            return self.exact[v], 'operand'
        if self.text_end <= v < self.image_end:
            d, note = self.data_shift(v)
            return (None if d is None else v + d), note
        if self.TEXT <= v < self.text_end:
            i = bisect.bisect_right(self.starts, v) - 1
            if i < 0 or not (self.f[i][0] <= v < self.f[i][1]):
                return None, 'outside any function'
            s, e, n = self.f[i]
            if n is None:
                return None, f'in unmatched sub_{s:08X}'
            if v != s and self.mk:
                _, _, starts = self.mk.masked(s, e - s)
                if v not in starts:
                    return v - s + n, f'mid-instruction in sub_{s:08X}'
            if v != s and self.mk2 and not self.same_prefix(s, n, v - s):
                return None, f'sub_{s:08X}+0x{v - s:X}: the PAL body differs before it'
            return v - s + n, 'start' if v == s else f'sub_{s:08X}+0x{v - s:X}'
        if v == self.text_end:
            return self.sec[0][0] + self.sec[0][1], '.text end'
        if v == self.image_end:
            return v + self.sec[-1][1], 'image end'
        return None, 'not an image address'


SUB = re.compile(r'\b(sub|loc)_([0-9A-Fa-f]{8})(?![0-9A-Fa-f])')
HEX = re.compile(r'\b0x([0-9A-Fa-f]{5,8})(?![0-9A-Fa-f])')
# a function address passed bare to a token-pasting macro: FALLBACK(001A89A2) -> sub_##addr##_gen
BARE = re.compile(r'(?<=\()(\s*)(00[0-9A-Fa-f]{6})(?=\s*[,)])')


def fmt(new, old_digits):
    s = f'{new:0{len(old_digits)}X}'
    return s if not old_digits.islower() or old_digits.isdigit() else s.lower()


def load_rules(path):
    """rules.txt: `keep 0xV` leaves a literal as it is, `map 0xNTSC 0xPAL` translates one the map cannot place."""
    keep, fixed = set(), {}
    if path:
        for l in open(path):
            l = l.split('#', 1)[0].split()
            if not l:
                continue
            if l[0] == 'keep':
                keep.add(int(l[1], 16))
            elif l[0] == 'map':
                fixed[int(l[1], 16)] = int(l[2], 16)
            else:
                sys.exit(f'{path}: unknown rule {l[0]!r}')
    return keep, fixed


def translate_text(text, T, keep, fixed, force_review=False):
    changes, review = [], []
    lo, hi = T.TEXT, T.image_end

    def line_of(pos):
        return text.count('\n', 0, pos) + 1

    def rep_sub(mo):
        v = int(mo.group(2), 16)
        if v in keep:
            return mo.group(0)
        n, note = (fixed[v], 'rules.txt') if v in fixed else T.tr(v)
        if n is None:
            if lo <= v < hi:
                review.append((line_of(mo.start()), mo.group(0), note))
            return mo.group(0)
        new = f'{mo.group(1)}_{fmt(n, mo.group(2))}'
        changes.append((line_of(mo.start()), mo.group(0), new, note))
        return new

    def rep_hex(mo):
        v = int(mo.group(1), 16)
        if not (lo <= v <= hi) or v in keep:
            return mo.group(0)
        if v in fixed:
            n, note, doubt = fixed[v], 'rules.txt', False
        else:
            n, note = T.tr(v)
            round_ = v & 0xFFF == 0
            doubt = n is None or note.startswith('mid-instruction') or round_
            if round_:
                note += ' (round number: size?)'
        if doubt and not force_review:
            review.append((line_of(mo.start()), mo.group(0), note))
            return mo.group(0)
        new = '0x' + fmt(n, mo.group(1))
        changes.append((line_of(mo.start()), mo.group(0), new, note))
        return new

    def rep_bare(mo):
        v = int(mo.group(2), 16)
        if not (lo <= v < hi) or v in keep:
            return mo.group(0)
        n, note = (fixed[v], 'rules.txt') if v in fixed else T.tr(v)
        if n is None:
            review.append((line_of(mo.start()), mo.group(2), note))
            return mo.group(0)
        changes.append((line_of(mo.start()), mo.group(2), fmt(n, mo.group(2)), note))
        return mo.group(1) + fmt(n, mo.group(2))

    out = SUB.sub(rep_sub, text)
    out = HEX.sub(rep_hex, out)
    out = BARE.sub(rep_bare, out)
    return out, changes, review


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    a = sp.add_parser('map')
    a.add_argument('ntsc'); a.add_argument('pal'); a.add_argument('functions'); a.add_argument('-o', required=True)
    t = sp.add_parser('translate')
    t.add_argument('map'); t.add_argument('files', nargs='+')
    t.add_argument('--ntsc', help='NTSC XBE: flags literals that are not instruction starts')
    t.add_argument('--pal', help='PAL XBE (with --ntsc): an address inside a function is placed only where both '
                                  'bodies match up to it')
    t.add_argument('--rules', help='rules.txt: literals to keep, and NTSC -> PAL pairs the map cannot place')
    t.add_argument('--root', help='with --out-dir: the files are written at their path relative to this')
    t.add_argument('--out-dir', help='write the PAL copies here instead of over the files')
    t.add_argument('--strict', action='store_true', help='fail on a literal neither translated nor in rules.txt')
    t.add_argument('--quiet', action='store_true', help='print only what needs review')
    t.add_argument('--dry-run', action='store_true')
    t.add_argument('--force-review', action='store_true', help='translate the REVIEW literals too')
    args = ap.parse_args()
    if args.cmd == 'map':
        json.dump(build_map(args.ntsc, args.pal, args.functions), open(args.o, 'w'))
        return
    T = Translator(json.load(open(args.map)), args.ntsc, args.pal)
    keep, fixed = load_rules(args.rules)
    bad = 0
    for p in args.files:
        text = open(p, encoding='utf-8', errors='surrogateescape').read()
        out, ch, rv = translate_text(text, T, keep, fixed, args.force_review)
        if not args.quiet:
            for ln, old, new, note in ch:
                print(f'{p}:{ln}: {old} -> {new}  [{note}]')
        for ln, old, note in rv:
            print(f'REVIEW {p}:{ln}: {old}  [{note}]', file=sys.stderr if args.strict else sys.stdout)
        bad += len(rv)
        if args.dry_run:
            continue
        dst = os.path.join(args.out_dir, os.path.relpath(p, args.root or '.')) if args.out_dir else p
        os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
        if args.out_dir and dst.endswith(('.c', '.cpp', '.h')):
            out = f'/* PAL copy of {os.path.relpath(p, args.root or ".")}, made by tools/edition/ntsc2pal.py: edit the NTSC source */\n' + out
        if not (os.path.exists(dst) and open(dst, encoding='utf-8', errors='surrogateescape').read() == out):
            open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(out)
    if args.strict and bad:
        sys.exit(f'ntsc2pal: {bad} literal(s) need a rule in tools/edition/rules.txt (keep or map)')


if __name__ == '__main__':
    main()
