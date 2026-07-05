"""
Copyright (C) 2024, Texas Instruments Incorporated

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the
    distribution.

    Neither the name of Texas Instruments Incorporated nor the names of
    its contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

"""Decode the LogSinkBuf circular byte buffer.

LogSinkBuf packs each log record into a circular buffer as a COBS frame
terminated by a 0x00 byte. After COBS-decoding, a printf frame is:

    [ id : 2 bytes little-endian ]   low 16 bits of the .log_ptr slot
    [ ts : ULEB128 ]                 timestamp delta from the previous record
    [ args... : ULEB128 each ]       promoted 32-bit printf arguments

and a Log_buf frame replaces the arguments with [ len : ULEB128 ][ len bytes ].

The host recovers the format string, argument count and everything else about the
log site from the .out file via the id (see tracedb.logIndexDB), so the target
keeps almost nothing in RAM. The target only advances ``wrReserve``; this decoder
reads it to find the write frontier, reads ``lastTs`` to anchor the timestamp
deltas to absolute time, and reads ``recCount`` (records ever committed) to count
records that were overwritten before the host read them.

The encode helpers mirror the target C in LogSinkBuf.c byte-for-byte, so they
double as a synthetic-data generator for the tests and as a cross-check that the C
and Python agree on the wire format.
"""

from collections import namedtuple

from tilogger.tracedb import LOG_ID_MASK, Opcode, format_c

# Buffer type constants, matching LogSinkBuf.h.
TYPE_LINEAR = 1
TYPE_CIRCULAR = 2

Record = namedtuple("Record", ["log_id", "elf", "delta", "timestamp", "args", "text"])
DecodeResult = namedtuple("DecodeResult", ["records", "decoded", "dropped", "torn", "rd"])


# ---------------------------------------------------------------------------
# ULEB128 and COBS primitives (encode mirrors the target C exactly)
# ---------------------------------------------------------------------------
def uleb_encode(value):
    """ULEB128-encode an unsigned integer."""
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value & 0x7F)
    return bytes(out)


def uleb_decode(data, pos):
    """Decode a ULEB128 value from data at pos. Returns (value, new_pos)."""
    result = 0
    shift = 0
    while True:
        if pos >= len(data):
            raise ValueError("truncated ULEB128")
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return result, pos
        shift += 7


def cobs_encode(payload):
    """COBS-encode payload and append the 0x00 frame delimiter."""
    out = bytearray()
    code_idx = len(out)
    out.append(0)  # placeholder for the first block's code byte
    code = 1
    for byte in payload:
        if byte != 0:
            out.append(byte)
            code += 1
            if code != 0xFF:
                continue
        out[code_idx] = code
        code_idx = len(out)
        out.append(0)
        code = 1
    out[code_idx] = code
    out.append(0)  # delimiter
    return bytes(out)


def cobs_decode(frame):
    """Decode a single COBS frame (without the trailing delimiter).

    Returns the decoded bytes, or None if the frame is malformed (a torn or
    overwritten record).
    """
    out = bytearray()
    i = 0
    n = len(frame)
    while i < n:
        code = frame[i]
        if code == 0:
            return None  # a 0x00 cannot appear inside a COBS frame
        i += 1
        block = code - 1
        if i + block > n:
            return None  # code points past the end of the frame
        out += frame[i : i + block]
        i += block
        if code < 0xFF and i < n:
            out.append(0)
    return bytes(out)


# ---------------------------------------------------------------------------
# Circular-buffer decode
# ---------------------------------------------------------------------------
def _find_delim(ring, cur, end):
    """Monotonic offset of the next 0x00 in [cur, end), or None."""
    size = len(ring)
    while cur < end:
        if ring[cur % size] == 0:
            return cur
        cur += 1
    return None


def _slice(ring, cur, end):
    """Bytes of the ring in [cur, end), reading modularly across the wrap."""
    size = len(ring)
    return bytes(ring[o % size] for o in range(cur, end))


def _format_record(payload, elf):
    """Parse one COBS-decoded payload into (delta, args, text). Raises on any
    inconsistency so the caller can count it as a torn frame."""
    delta, pos = uleb_decode(payload, 2)

    if elf.opcode == Opcode.BUFFER:
        length, pos = uleb_decode(payload, pos)
        data = payload[pos : pos + length]
        if len(data) != length:
            raise ValueError("buffer payload shorter than its length field")
        text = elf.string + " ".join("0x%x" % b for b in data)
        return delta, list(data), text

    args = []
    while pos < len(payload):
        value, pos = uleb_decode(payload, pos)
        args.append(value)
    text = format_c(elf.string, args) if elf.nargs else elf.string
    return delta, args, text


def decode_buffer(ring, wr_reserve, last_ts, rec_count, log_index, rd=None, decoded_prior=0):
    """Decode the committed records in a LogSinkBuf circular buffer.

    ring:        raw buffer bytes (length == the target's ``size``).
    wr_reserve:  the target's monotonic write offset.
    last_ts:     the target's ``lastTs`` (absolute time of the most recent record).
    rec_count:   the target's ``recCount`` (records ever committed).
    log_index:   {id -> ElfString}, e.g. TraceDB.logIndexDB.
    rd:          monotonic read offset to start at. Defaults to the start of the
                 newest full window (post-mortem / fresh snapshot).
    decoded_prior: records this host already decoded in earlier calls, so drops
                 can be counted across a streaming session.

    Returns DecodeResult(records, decoded, dropped, torn, rd) where ``rd`` is the
    new read offset to pass back on the next call.
    """
    size = len(ring)

    if rd is None:
        # Fresh snapshot: the newest window is all that survives. If the buffer has
        # wrapped, its oldest byte lands mid-frame, so we must realign.
        if wr_reserve > size:
            rd = wr_reserve - size
            resync = True
        else:
            rd = 0
            resync = False
    else:
        # If the target lapped us, everything older than one buffer is gone. Jump
        # to the newest window; recCount vs decoded still counts what was lost.
        resync = wr_reserve - rd > size
        if resync:
            rd = wr_reserve - size

    cur = rd
    torn = 0
    records = []

    # A window that starts mid-frame is realigned by discarding bytes up to the
    # first delimiter. That partial is the overwrite seam, not a decode error.
    if resync:
        delim = _find_delim(ring, cur, wr_reserve)
        if delim is None:
            dropped = max(0, rec_count - decoded_prior)
            return DecodeResult([], 0, dropped, 0, wr_reserve)
        cur = delim + 1

    while cur < wr_reserve:
        delim = _find_delim(ring, cur, wr_reserve)
        if delim is None:
            break  # frame at the frontier is still being written; retry next poll
        frame = _slice(ring, cur, delim)
        cur = delim + 1

        if len(frame) == 0:
            continue  # empty frame: reservation padding, skip silently

        payload = cobs_decode(frame)
        if payload is None or len(payload) < 3:
            torn += 1
            continue

        log_id = payload[0] | (payload[1] << 8)
        elf = log_index.get(log_id & LOG_ID_MASK)
        if elf is None:
            torn += 1
            continue

        try:
            delta, args, text = _format_record(payload, elf)
        except (ValueError, TypeError):
            torn += 1
            continue

        records.append(Record(log_id, elf, delta, None, args, text))

    # Anchor absolute time: the most recent record is at last_ts, then walk back
    # subtracting each record's own delta.
    if records:
        ts = last_ts
        for i in range(len(records) - 1, -1, -1):
            records[i] = records[i]._replace(timestamp=ts)
            ts = (ts - records[i].delta) & 0xFFFFFFFF

    decoded = len(records)

    # If we stopped before the frontier, one record has been reserved (and counted
    # in recCount) but its body is still being written. It is not lost - it will
    # decode on the next poll - so exclude it from the drop count.
    inflight = 1 if cur < wr_reserve else 0
    dropped = max(0, rec_count - decoded_prior - decoded - inflight)
    return DecodeResult(records, decoded, dropped, torn, cur)


# ---------------------------------------------------------------------------
# RingWriter: a Python mirror of the target enqueue, for synthetic test data
# ---------------------------------------------------------------------------
class RingWriter:
    """Mirror of the LogSinkBuf.c enqueue path, so tests can generate exactly the
    bytes the target would write."""

    def __init__(self, size, buf_type=TYPE_CIRCULAR):
        self.buf = bytearray(size)
        self.size = size
        self.buf_type = buf_type
        self.wr_reserve = 0
        self.last_ts = 0
        self.rec_count = 0
        self.overflow = 0

    def _delta(self, now):
        if now >= self.last_ts:
            delta = now - self.last_ts
            self.last_ts = now
        else:
            delta = 0
        return delta

    def _put(self, off, data):
        for i, byte in enumerate(data):
            self.buf[(off + i) % self.size] = byte

    def _reserve(self, length):
        off = self.wr_reserve
        if length > self.size:
            return None
        if self.buf_type == TYPE_LINEAR and (off + length) > self.size:
            self.overflow += 1
            return None
        self.wr_reserve = off + length
        self.rec_count += 1
        return off

    def printf(self, log_id, now, args=()):
        delta = self._delta(now)
        payload = bytes([log_id & 0xFF, (log_id >> 8) & 0xFF]) + uleb_encode(delta)
        for arg in args:
            payload += uleb_encode(arg & 0xFFFFFFFF)
        frame = cobs_encode(payload)
        off = self._reserve(len(frame))
        if off is None:
            return
        self._put(off, frame)

    def log_buf(self, log_id, now, data):
        delta = self._delta(now)
        payload_len = 2 + len(uleb_encode(delta)) + len(uleb_encode(len(data))) + len(data)
        worst = payload_len + payload_len // 254 + 2
        off = self._reserve(worst)
        if off is None:
            return
        payload = bytes([log_id & 0xFF, (log_id >> 8) & 0xFF]) + uleb_encode(delta) + uleb_encode(len(data)) + bytes(data)
        frame = cobs_encode(payload)
        self._put(off, frame)
        # Pad the reserved tail with delimiters, exactly as the target does.
        for i in range(len(frame), worst):
            self.buf[(off + i) % self.size] = 0
