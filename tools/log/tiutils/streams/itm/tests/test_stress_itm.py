"""Stress and robustness tests for the ITM framer: it must never raise on
arbitrary wire data, must recover from corruption, and must bound its own
memory while waiting to align on a running target."""

import os
import random

import synth
from conftest import ListSink, drain

from tilogger_itm_transport.itm_framer import (
    ITM_SYNC_MIN_ZEROS,
    ITMFramer,
    ITMOpcode,
)


# ---------------------------------------------------------------------------
# Never crash on arbitrary input
# ---------------------------------------------------------------------------
def test_fuzz_random_bytes_never_crash():
    """Arbitrary bytes - with or without a reset token, at any chunking - must
    decode to *something* without ever raising. This is the load-bearing
    guarantee: a garbled SWO line cannot take the host down."""
    rng = random.Random(1234)
    for _ in range(400):
        n = rng.randint(0, 512)
        blob = bytes(rng.getrandbits(8) for _ in range(n))
        if rng.random() < 0.5:
            blob = synth.RESET + blob  # start synced, then feed garbage
        framer = ITMFramer(ListSink(), late_attach=rng.random() < 0.5)
        # Random chunk boundaries stress the tail-carry paths too.
        leftover = bytearray()
        i = 0
        while i < len(blob):
            step = rng.randint(1, 9)
            leftover.extend(blob[i : i + step])
            leftover = framer.parse(leftover)
            i += step


def test_fuzz_urandom_smoke():
    """A few kilobytes of true entropy through the byte-at-a-time path."""
    blob = synth.RESET + os.urandom(4096)
    framer = ITMFramer(ListSink())
    leftover = bytearray()
    for b in blob:
        leftover.append(b)
        leftover = framer.parse(leftover)


# ---------------------------------------------------------------------------
# Corruption recovery: a torn frame does not desync the whole stream
# ---------------------------------------------------------------------------
def test_corrupt_middle_recovers_on_next_reset():
    """Garbage in the middle of a synced stream may mis-decode a few frames,
    but a later reset token re-aligns and the tail decodes cleanly."""
    sink = ListSink()
    framer = ITMFramer(sink)
    framer.parse(bytearray(synth.RESET + synth.pc_sample(0x11) + os.urandom(37)))
    before = len(sink)
    framer.parse(bytearray(synth.RESET + synth.pc_sample(0x22) + b"\xff" * 5))
    pcs = [f for f in sink[before:] if f.opcode == ITMOpcode.PACKET_PC]
    assert pcs and pcs[-1].value == 0x22


def test_invalid_header_resyncs_on_next_byte():
    """An unknown header (length unknowable) is dropped and parsing continues
    at the next byte; the following valid frame still decodes."""
    # 0x74: low2==0, not sync/overflow/timestamp/extension/global -> the
    # 'invalid header' branch.
    stream = synth.RESET + bytes([0x74]) + synth.pc_sample(0x99) + b"\xff" * 5
    frames = drain(stream)
    assert any(f.opcode == ITMOpcode.PACKET_PC and f.value == 0x99 for f in frames)


# ---------------------------------------------------------------------------
# Multi-byte variable-length packets (the deferred-tail branches)
# ---------------------------------------------------------------------------
def test_multibyte_extension_packet_keeps_sync():
    """A continued (multi-byte) extension packet is consumed whole so the
    following frame stays aligned."""
    # 0x88 = extension (bits 0b1000) with continuation bit set; 0x81 continues,
    # 0x00 ends.
    stream = synth.RESET + bytes([0x88, 0x81, 0x00]) + synth.pc_sample(0x55) + b"\xff" * 5
    frames = drain(stream)
    assert any(f.opcode == ITMOpcode.EXTENSION for f in frames)
    assert any(f.opcode == ITMOpcode.PACKET_PC and f.value == 0x55 for f in frames)


def test_global_timestamp_multibyte_keeps_sync():
    """A multi-byte global timestamp (GTS2, header 0xB4) is consumed whole."""
    stream = synth.RESET + bytes([0xB4, 0x81, 0x82, 0x03]) + synth.pc_sample(0x66) + b"\xff" * 5
    frames = drain(stream)
    assert any(f.opcode == ITMOpcode.GLOBAL_TIMESTAMP for f in frames)
    assert any(f.opcode == ITMOpcode.PACKET_PC and f.value == 0x66 for f in frames)


def test_global_timestamp_split_across_reads_defers():
    """A global timestamp whose continuation bytes have not all arrived is held,
    not misparsed (the incomplete-defer path)."""
    sink = ListSink()
    framer = ITMFramer(sink)
    # Deliver header + one continuation byte (bit7 set: more to come).
    leftover = framer.parse(bytearray(synth.RESET + bytes([0xB4, 0x81])))
    # Now the rest arrives.
    leftover.extend(bytes([0x82, 0x03]) + synth.pc_sample(0x77) + b"\xff" * 5)
    framer.parse(leftover)
    assert any(f.opcode == ITMOpcode.GLOBAL_TIMESTAMP for f in sink)
    assert any(f.opcode == ITMOpcode.PACKET_PC and f.value == 0x77 for f in sink)


# ---------------------------------------------------------------------------
# Late-attach memory is bounded while it waits for a marker
# ---------------------------------------------------------------------------
def test_late_attach_garbage_flood_stays_bounded_then_aligns():
    """A late attach to a target that never sends a marker must not accumulate
    the whole stream in memory; it keeps only a short tail across an unbounded
    garbage flood. Once a sync packet finally arrives it aligns and decodes."""
    sink = ListSink()
    framer = ITMFramer(sink, late_attach=True)
    rng = random.Random(99)
    leftover = bytearray()
    for _ in range(400):
        # Garbage with no zero-run and no 0xFB/0xBB, so it can neither look like
        # a sync marker nor a (partial) reset token: exercises the trim path.
        leftover.extend(rng.randint(1, 0xAA) for _ in range(512))
        leftover = framer.parse(leftover)
        assert sink == []  # nothing spurious decoded
        assert len(leftover) < ITM_SYNC_MIN_ZEROS  # bounded: only a short tail
    # A sync packet plus a frame finally lands.
    leftover.extend(synth.sync_packet(47, terminator=0x80) + synth.pc_sample(0xABCD) + b"\xff" * 5)
    framer.parse(leftover)
    assert any(f.opcode == ITMOpcode.PACKET_PC and f.value == 0xABCD for f in sink)
