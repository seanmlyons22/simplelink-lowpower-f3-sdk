#!/usr/bin/env python3
"""Python port of the tracedecode decode path, for the decoder-language decision.

This file exists to keep the Rust-vs-Python question answerable by measurement
instead of opinion (see trace-pipeline-architecture.md.mkd, "Decoder language").
It reimplements the full hot path - sample ingest, deframe, packet reassembly,
CRC-5, timestamp reconstruction, dbgid resolve, printf - and must produce output
byte-identical to `tracedecode` on the same inputs (the Rust test suite asserts
this cross-check on synthetic captures).

Two deframe engines:
  default   pure Python (stdlib only)
  --numpy   NumPy-vectorized edge extraction and bit-center sampling; falls back
            to the exact scalar core for any window containing a framing error,
            so its output stays identical to the pure path

CLI subset (mirrors tracedecode where implemented):
  pydecode.py decode --raw <file|-> --channel N --dbgid F [--dbgid F2 ...]
              [--samplerate HZ] [--baud HZ] [--alias NAME] [--divide-time-by-2]
              [--numpy] stdout
  pydecode.py replay <file.sal> --channel N --dbgid F ... [--numpy] stdout
"""

import json
import math
import re
import sys
import zipfile
from array import array

# ---------------------------------------------------------------- dbgid ----


def _split_top_level(s):
    """Split on commas outside double-quoted strings; honors backslash escapes."""
    out, cur, in_q, prev_bs = [], [], False, False
    for c in s:
        if c == '"' and not prev_bs:
            in_q = not in_q
            cur.append(c)
        elif c == "," and not in_q:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        prev_bs = c == "\\" and not prev_bs
    out.append("".join(cur))
    return out


def _unquote(s):
    s = s.strip()
    if s.startswith('"'):
        s = s[1:]
    if s.endswith('"'):
        s = s[:-1]
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}.get(nxt, "\\" + nxt))
            i += 2 if nxt in 'ntr"\\' else 1
            if nxt not in 'ntr"\\':
                continue
        else:
            out.append(c)
            i += 1
    return "".join(out)


def parse_dbgid_line(line):
    line = line.strip()
    if not line.startswith("DBG_DEF"):
        return None
    rest = line[len("DBG_DEF"):].lstrip()
    if not rest.startswith("(") or not rest.rstrip().endswith(")"):
        return None
    fields = _split_top_level(rest.rstrip()[1:-1])
    if len(fields) != 7:
        return None
    try:
        dbgid = int(fields[1].strip())
        ch_field = fields[2].strip()
        if not ch_field.startswith("DBGCH"):
            return None
        channel = int(ch_field[5:].strip())
        arg_count = int(fields[3].strip())
        lineno = int(fields[6].strip())
    except ValueError:
        return None
    if not (3 <= dbgid <= 255 and 1 <= channel <= 3):
        return None
    fmt = _unquote(fields[4].strip())
    fname = _unquote(fields[5].strip())
    base = re.split(r"[/\\]", fname)[-1]
    return (channel, dbgid, arg_count, fmt, base, lineno)


def load_dbgid(paths):
    db = {}
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            for line in f:
                d = parse_dbgid_line(line)
                if d:
                    db[(d[0], d[1])] = d
    return db


# ---------------------------------------------------------------- crc5 ----

# MSB-first CRC-5, poly 0x05, init 0x1F: precomputed per (reg, byte) as a flat
# table so packet assembly costs one lookup per byte.
def _crc5_byte(reg, byte):
    for i in range(7, -1, -1):
        fb = ((reg >> 4) & 1) ^ ((byte >> i) & 1)
        reg = (reg << 1) & 0x1F
        if fb:
            reg ^= 0x05
    return reg


_CRC5_TBL = [_crc5_byte(r, b) for r in range(32) for b in range(256)]


def _crc5_bits(reg, val, n):
    for i in range(n - 1, -1, -1):
        fb = ((reg >> 4) & 1) ^ ((val >> i) & 1)
        reg = (reg << 1) & 0x1F
        if fb:
            reg ^= 0x05
    return reg


# ---------------------------------------------------------------- printf ----


def _fmt_int(v, prec):
    neg = v < 0
    digits = str(-v if neg else v)
    if prec is not None:
        digits = digits.rjust(prec, "0")
    return "-" + digits if neg else digits


def _fmt_uint(v, base, upper, prec):
    if v == 0 and prec == 0:
        return ""
    if base == 10:
        s = str(v)
    elif base == 16:
        s = format(v, "X" if upper else "x")
    else:
        s = format(v, "o")
    if prec is not None:
        s = s.rjust(prec, "0")
    return s


def _pad(s, width, has_w, left, zero):
    if not has_w or len(s) >= width:
        return s
    fill = width - len(s)
    if left:
        return s + " " * fill
    if zero:
        if s.startswith("-"):
            return "-" + "0" * fill + s[1:]
        return "0" * fill + s
    return " " * fill + s


def format_c(fmt, args):
    """C printf subset identical to the Rust `record::format_c`."""
    out = []
    i, ai, n = 0, 0, len(fmt)
    while i < n:
        c = fmt[i]
        if c != "%":
            out.append(c)
            i += 1
            continue
        i += 1
        if i < n and fmt[i] == "%":
            out.append("%")
            i += 1
            continue
        left = zero = False
        while i < n and fmt[i] in "-+ #0":
            if fmt[i] == "-":
                left = True
            if fmt[i] == "0":
                zero = True
            i += 1
        width, has_w = 0, False
        while i < n and fmt[i].isdigit():
            width = width * 10 + int(fmt[i])
            has_w = True
            i += 1
        prec = None
        if i < n and fmt[i] == ".":
            i += 1
            prec = 0
            while i < n and fmt[i].isdigit():
                prec = prec * 10 + int(fmt[i])
                i += 1
        while i < n and fmt[i] in "lhzjtL":
            i += 1
        if i >= n:
            out.append("%")
            break
        conv = fmt[i]
        i += 1
        arg = args[ai] if ai < len(args) else 0
        consumed = True
        if conv in "di":
            body = _fmt_int(arg - (1 << 32) if arg >= (1 << 31) else arg, prec)
        elif conv == "u":
            body = _fmt_uint(arg, 10, False, prec)
        elif conv == "x":
            body = _fmt_uint(arg, 16, False, prec)
        elif conv == "X":
            body = _fmt_uint(arg, 16, True, prec)
        elif conv == "o":
            body = _fmt_uint(arg, 8, False, prec)
        elif conv == "p":
            body = "0x%x" % arg
        elif conv == "c":
            body = chr(arg & 0xFF)
        elif conv == "s":
            body = "0x%X" % arg  # the tracer has no string args; show the raw value
        else:
            consumed = False
            body = "%" + conv
        if consumed:
            ai += 1
        out.append(_pad(body, width, has_w, left, zero))
    return "".join(out)


# ------------------------------------------------- packet assembly + resolve ----


class Health:
    __slots__ = ("framing", "crc", "overflow", "unknown", "dropped_seq")

    def __init__(self):
        self.framing = self.crc = self.overflow = self.unknown = self.dropped_seq = 0


class Decoder:
    """Word -> record pipeline: classify, per-channel state machines, CRC-5,
    timestamp unwrap, dbgid resolve, printf. Mirrors the Rust modules exactly."""

    def __init__(self, db, alias, divide_time_by_2, out=sys.stdout):
        self.db = db
        self.alias = alias
        self.div2 = divide_time_by_2
        self.health = Health()
        self.out = out
        # per channel 1..3: [active, ts_en, seq, expect, ts_hi, ts_lo, dbgid, data, crc]
        self.ch = [None, self._idle(), self._idle(), self._idle()]
        self.last_seq = [None, None, None, None]
        self.ts_last = 0
        self.ts_have = False

    @staticmethod
    def _idle():
        return {"active": False}

    def _ts_reconstruct(self, delta16):
        if not self.ts_have:
            self.ts_have = True
            self.ts_last = delta16
            return delta16
        val = (self.ts_last & ~0xFFFF) | delta16
        if val + 2000 < self.ts_last:
            val += 0x10000
        self.ts_last = val
        return val

    def feed_word(self, w):
        chx = (w >> 8) & 0x3
        if chx:
            st = self.ch[chx]
            if not st["active"]:
                return
            byte = w & 0xFF
            st["crc"] = _CRC5_TBL[(st["crc"] << 8) | byte]
            exp = st["expect"]
            if exp == 0:  # ts high
                st["ts_hi"] = byte
                st["expect"] = 1
            elif exp == 1:  # ts low
                st["ts_lo"] = byte
                st["expect"] = 2
            elif exp == 2:  # header = dbgid
                st["dbgid"] = byte
                st["expect"] = 3
            else:
                st["data"].append(byte)
            return
        ch76 = (w >> 6) & 0x3
        if w & 0x20:
            if ch76:
                self._eop(ch76, w & 0x1F)
            # TS-MSB: consumed as state upstream in Rust; nothing to do here
            return
        if ch76:
            st = self.ch[ch76]
            st.clear()
            st.update(
                active=True,
                ts_en=bool(w & 0x10),
                seq=w & 0xF,
                expect=0 if (w & 0x10) else 2,
                ts_hi=0,
                ts_lo=0,
                dbgid=0,
                data=bytearray(),
                crc=_CRC5_TBL[(0x1F << 8) | ((ch76 << 6) | (w & 0x10) | (w & 0xF))],
            )
            return
        if w & 0x00E:
            self.health.overflow += 1

    def _eop(self, ch, crc5):
        st = self.ch[ch]
        if not st["active"]:
            return
        st["active"] = False
        crc = _crc5_bits(st["crc"], ((ch << 1) | 1) & 0x7, 3)
        if crc != crc5:
            self.health.crc += 1
            return
        prev = self.last_seq[ch]
        if prev is not None:
            self.health.dropped_seq += (st["seq"] + 16 - prev - 1) & 0xF
        self.last_seq[ch] = st["seq"]
        d = self.db.get((ch, st["dbgid"]))
        if d is None:
            self.health.unknown += 1
            return
        data = st["data"]
        params = [
            (data[i] << 8) | (data[i + 1] if i + 1 < len(data) else 0)
            for i in range(0, len(data), 2)
        ]
        _, _, arg_count, fmt, base, lineno = d
        if arg_count < 0:
            args = [
                params[i] | ((params[i + 1] if i + 1 < len(params) else 0) << 16)
                for i in range(0, len(params), 2)
            ]
        else:
            args = params
        if st["ts_en"]:
            ticks = self._ts_reconstruct((st["ts_hi"] << 8) | st["ts_lo"])
        else:
            ticks = self.ts_last
        us = ticks / 2.0
        if self.div2:
            us /= 2.0
        text = format_c(fmt, args)
        self.out.write(
            f"{self.alias} | {us / 1_000_000.0:.9f} | DBGCH{ch} | INFO | {base}:{lineno} | {text}\n"
        )

    def print_health(self):
        h = self.health
        print(
            f"[health] framing={h.framing} crc={h.crc} overflow={h.overflow} "
            f"unknown_dbgid={h.unknown} dropped_seq={h.dropped_seq}",
            file=sys.stderr,
        )


# ---------------------------------------------------------------- deframe ----


class Cfg:
    def __init__(self, samplerate=500e6, baud=24e6, data_bits=10):
        self.samplerate = samplerate
        self.baud = baud
        self.data_bits = data_bits

    @property
    def spb(self):
        return self.samplerate / self.baud


def minority_start_level(initial, edges, total, invert_hint=True):
    if total == 0:
        return 0 if invert_hint else 1
    hi, lvl, prev = 0, initial & 1, 0
    for e in edges:
        if lvl:
            hi += e - prev
        prev = e
        lvl ^= 1
    if lvl:
        hi += total - prev
    if hi * 2 > total:
        return 0
    if hi * 2 < total:
        return 1
    return 0 if invert_hint else 1


def deframe_edges_core(initial, edges, total, cfg, start_level):
    """Exact scalar port of the Rust deframer: returns (words, framing_errors, consumed)."""
    spb = cfg.spb
    bpf = 2 + cfg.data_bits
    words = array("H")
    fe = 0
    if total == 0:
        return words, fe, 0
    cur = 0
    nedges = len(edges)
    frame_bits = [0] * bpf
    pos_limit = 0.0
    fspan = (bpf - 0.5) * spb
    data_bits = cfg.data_bits
    for i in range(nedges + 1):
        s = 0 if i == 0 else edges[i - 1]
        if (initial ^ (i & 1)) != start_level or s < pos_limit:
            continue
        sf = float(s)
        if sf + fspan > total:
            break
        for k in range(bpf):
            pos = sf + (k + 0.5) * spb
            while cur < nedges and edges[cur] <= pos:
                cur += 1
            frame_bits[k] = 1 if (initial ^ (cur & 1)) == start_level else 0
        if frame_bits[bpf - 1]:
            fe += 1
            pos_limit = sf + 1.0
            while cur > 0 and edges[cur - 1] > pos_limit:
                cur -= 1
            continue
        w = 0
        for b in frame_bits[1:1 + data_bits]:
            w = (w << 1) | b
        words.append(w)
        pos_limit = sf + fspan
        while cur > 0 and edges[cur - 1] > pos_limit:
            cur -= 1
    return words, fe, int(pos_limit)


def edges_from_levels(levels):
    """Transition list from a 0/1 bytes object, via C-speed bytes.find."""
    edges = array("q")
    cur = levels[0]
    i = 0
    while True:
        j = levels.find(1 - cur, i)
        if j < 0:
            return edges
        edges.append(j)
        cur = 1 - cur
        i = j


def deframe_edges_numpy(initial, edges_np, total, cfg, start_level):
    """NumPy-vectorized deframe. Identical output to the scalar core on clean input;
    any stop-bit violation falls back to the scalar core for the whole window."""
    import numpy as np

    spb = cfg.spb
    bpf = 2 + cfg.data_bits
    fspan = (bpf - 0.5) * spb
    if total == 0 or len(edges_np) == 0:
        return array("H"), 0, 0
    # Candidate frame starts: runs at the start level. Run i starts at
    # (0 if i == 0 else edges[i-1]) with level initial ^ (i & 1).
    starts = np.empty(len(edges_np) + 1, dtype=np.int64)
    starts[0] = 0
    starts[1:] = edges_np
    first = 0 if initial == start_level else 1
    cand = starts[first::2]
    # Greedy accept: skip candidates inside an accepted frame (in-frame data edges
    # at the start level are not new frames). Plain-int loop; each iteration is a
    # compare + append, so this is the cheap part of the chunk.
    acc = array("q")
    limit = 0.0
    for s in cand.tolist():
        if s < limit:
            continue
        if s + fspan > total:
            break
        acc.append(s)
        limit = s + fspan
    if not len(acc):
        return array("H"), 0, int(limit)
    accn = np.frombuffer(acc, dtype=np.int64)
    centers = (np.arange(bpf, dtype=np.float64) + 0.5) * spb
    pos = accn[:, None] + centers[None, :]
    counts = np.searchsorted(edges_np, pos, side="right")
    raw = initial ^ (counts & 1)
    bits = (raw == start_level).astype(np.uint16)
    if bits[:, -1].any():
        return None  # framing error present: caller reruns the exact scalar core
    weights = (1 << np.arange(cfg.data_bits - 1, -1, -1, dtype=np.uint16))
    words_np = (bits[:, 1:1 + cfg.data_bits] * weights).sum(axis=1, dtype=np.uint16)
    words = array("H")
    words.frombytes(words_np.astype("<u2").tobytes())
    return words, 0, int(limit)


# ---------------------------------------------------------------- drivers ----


CHUNK = 1 << 20


def stream_decode(reader, cfg, dec, channel, use_numpy):
    """Chunked streaming decode with the same one-frame carry as the Rust path."""
    np = None
    if use_numpy:
        import numpy as np  # noqa: F811
    bpf = 2 + cfg.data_bits
    frame_span = math.ceil(bpf * cfg.spb) + 1
    tbl = bytes(((b >> channel) & 1) for b in range(256))
    carry = b""
    start_level = None
    feed = dec.feed_word
    while True:
        chunk = reader.read(CHUNK)
        if not chunk:
            break
        window = carry + chunk.translate(tbl)
        total = len(window)
        if use_numpy:
            arr = np.frombuffer(window, dtype=np.uint8)
            edges_np = (np.flatnonzero(arr[1:] != arr[:-1]) + 1).astype(np.int64)
            edges = edges_np
        else:
            edges = edges_from_levels(window)
        initial = window[0]
        if start_level is None:
            sl = minority_start_level(initial, edges, total)
            if len(edges) >= 64:
                start_level = sl
        else:
            sl = start_level
        if use_numpy:
            res = deframe_edges_numpy(initial, edges_np, total, cfg, sl)
            if res is None:  # framing error: exact scalar core decides
                res = deframe_edges_core(initial, edges_np.tolist(), total, cfg, sl)
            words, fe, consumed = res
        else:
            words, fe, consumed = deframe_edges_core(initial, edges, total, cfg, sl)
        dec.health.framing += fe
        for w in words:
            feed(w)
        cons = min(consumed, total)
        new_carry = window[cons:]
        # A static tail can't hold a frame; cap the carry so idle lines stay O(1).
        if len(new_carry) > 4 * frame_span:
            has_tail_edge = (
                bool(len(edges_np) and int(edges_np[-1]) >= cons)
                if use_numpy
                else bool(len(edges) and edges[-1] >= cons)
            )
            if not has_tail_edge:
                new_carry = new_carry[-frame_span:]
        carry = new_carry


def read_sal(path, channel):
    """Saleae .sal: ZIP of meta.json + per-channel digital-N.bin transition lists."""
    with zipfile.ZipFile(path) as z:
        meta = json.loads(z.read("meta.json"))
        rate = float(meta.get("data", {}).get("sampleRate", {}).get("digital", 500e6))
        bin_ = z.read(f"digital-{channel}.bin")
    if bin_[:8] != b"<SALEAE>":
        raise ValueError("not a Saleae digital .bin")
    pos = 0x33
    edges = array("q")
    initial = 0
    cur_level = None
    expect_begin = 0
    import struct

    while pos + 48 <= len(bin_):
        begin, end, nsamples = struct.unpack_from("<QQQ", bin_, pos)
        (payload_len,) = struct.unpack_from("<Q", bin_, pos + 40)
        if nsamples != end - begin or begin != expect_begin:
            raise ValueError(f"inconsistent block header at {pos}")
        payload = bin_[pos + 48:pos + 48 + payload_len]
        tpos = pos + 48 + payload_len
        (n_idx,) = struct.unpack_from("<I", bin_, tpos)
        (block_level,) = struct.unpack_from("<I", bin_, tpos + 8 + 16)
        block_level &= 1
        if cur_level is None:
            initial = block_level
        elif cur_level != block_level:
            edges.append(begin)
        s = begin
        i = 0
        nruns = 0
        plen = len(payload)
        while i < plen:
            b = payload[i]
            i += 1
            v = b & 0x3F
            more = b & 0x40
            while more:
                c = payload[i]
                i += 1
                v = (v << 7) | (c & 0x7F)
                more = c & 0x80
            s += v + 1
            nruns += 1
            if s < end:
                edges.append(s)
        if s != end:
            raise ValueError(f"block at {pos}: bad run sum")
        cur_level = block_level ^ ((max(nruns, 1) - 1) & 1)
        expect_begin = end
        pos = tpos + 8 + 20 * n_idx
    return rate, initial, edges, expect_begin


def main(argv):
    # --numpy may appear anywhere, including before the subcommand (which lets a
    # benchmark driver select the engine via `DECODE="pydecode.py --numpy"`).
    use_numpy = "--numpy" in argv
    argv = [a for a in argv if a != "--numpy"]
    if len(argv) < 1 or argv[0] not in ("decode", "replay"):
        print(__doc__, file=sys.stderr)
        return 2
    sub = argv[0]
    args = argv[1:]
    raw = None
    sal = None
    dbgid = []
    channel = 4
    alias = "rftrc"
    samplerate = None
    baud = None
    div2 = False
    i = 0
    while i < len(args):
        t = args[i]
        if t == "--raw":
            i += 1
            raw = args[i]
        elif t == "--dbgid":
            i += 1
            dbgid.append(args[i])
        elif t == "--channel":
            i += 1
            channel = int(args[i])
        elif t == "--alias":
            i += 1
            alias = args[i]
        elif t == "--samplerate":
            i += 1
            samplerate = float(args[i])
        elif t == "--baud":
            i += 1
            baud = float(args[i])
        elif t == "--divide-time-by-2":
            div2 = True
        elif t == "stdout":
            pass
        elif not t.startswith("--") and sal is None and sub == "replay":
            sal = t
        else:
            print(f"error: unknown arg: {t}", file=sys.stderr)
            return 2
        i += 1

    db = load_dbgid(dbgid)
    print(f"[info] dbgid entries: {len(db)}", file=sys.stderr)
    dec = Decoder(db, alias, div2)

    if sub == "replay":
        rate, initial, edges, total = read_sal(sal, channel)
        cfg = Cfg(samplerate or rate, baud or 24e6)
        sl = minority_start_level(initial, edges, total)
        if use_numpy:
            import numpy as np

            edges_np = np.frombuffer(edges, dtype=np.int64)
            res = deframe_edges_numpy(initial, edges_np, total, cfg, sl)
            if res is None:
                res = deframe_edges_core(initial, edges, total, cfg, sl)
        else:
            res = deframe_edges_core(initial, edges, total, cfg, sl)
        words, fe, _ = res
        dec.health.framing += fe
        for w in words:
            dec.feed_word(w)
    else:
        cfg = Cfg(samplerate or 500e6, baud or 24e6)
        reader = sys.stdin.buffer if raw in (None, "-") else open(raw, "rb")
        try:
            stream_decode(reader, cfg, dec, channel, use_numpy)
        finally:
            if reader is not sys.stdin.buffer:
                reader.close()
    dec.print_health()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
