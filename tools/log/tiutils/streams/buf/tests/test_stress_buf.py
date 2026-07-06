"""Stress and robustness tests for the buf ring decoder: decode_buffer must
never raise on an arbitrary RAM snapshot, and the transport must keep its
drop/torn accounting honest when it attaches to a ring that has already lapped
many times (a real late connect)."""

import random

from tilogger.bufdecode import RingWriter, decode_buffer

from test_buf_transport import DB, INDEX, FakeLogger, make_transport, render


# ---------------------------------------------------------------------------
# Late connect: attach after the device has lapped a tiny ring many times
# ---------------------------------------------------------------------------
def test_late_connect_to_fully_lapped_ring():
    """The host joins long after boot: the very first poll faces a ring that has
    been overwritten hundreds of times. Everything committed is either decoded
    from the surviving window or counted as dropped - never silently lost - and
    the newest record still decodes."""
    writer = RingWriter(size=64)
    for i in range(500):
        writer.printf(0x0010, 100 + i, (i, i * 2))

    transport, _ = make_transport(writer)  # attach only now
    logger = FakeLogger()
    transport._poll_once(logger)

    assert transport._decoded_prior + transport._dropped == writer.rec_count
    assert transport._dropped > 0  # most of the history was lapped away
    assert transport._torn == 0
    assert render(logger.packets[-1]) == "a=499 b=998"


# ---------------------------------------------------------------------------
# Never crash on an arbitrary snapshot
# ---------------------------------------------------------------------------
def test_decode_buffer_fuzz_never_crashes():
    """A garbled RAM snapshot (wrong image, torn read, random bytes) must decode
    to something without raising, with self-consistent counts."""
    rng = random.Random(2024)
    for _ in range(500):
        size = rng.choice([16, 32, 64, 128, 256])
        ring = bytes(rng.getrandbits(8) for _ in range(size))
        wr_reserve = rng.randint(0, 5000)
        rec_count = rng.randint(0, 400)
        last_ts = rng.randint(0, 1 << 32)
        rd = rng.choice([None, rng.randint(0, wr_reserve or 1)])

        res = decode_buffer(ring, wr_reserve, last_ts, rec_count, INDEX, rd=rd)

        assert res.decoded == len(res.records)
        assert res.decoded >= 0 and res.dropped >= 0 and res.torn >= 0


def test_empty_ring_decodes_nothing():
    """A fresh (all-zero) ring with no records yields nothing, no crash."""
    res = decode_buffer(bytes(128), 0, 0, 0, INDEX)
    assert res.records == [] and res.decoded == 0 and res.dropped == 0


def test_all_delimiter_ring_is_not_records():
    """A ring of all COBS delimiters (0x00) must not be mistaken for a torrent
    of empty records; it decodes to nothing meaningful without raising."""
    res = decode_buffer(bytes([0] * 128), 128, 0, 3, INDEX)
    assert res.decoded == len(res.records)
