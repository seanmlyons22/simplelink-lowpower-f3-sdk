"""Unit tests for the native XDS110 RAM reader (streams/buf).

No hardware and no real USB: a FakeLink records the request bytes the reader
emits and hands back canned firmware responses. The load-bearing checks are
(1) the exact marshalled DAP request for a read that crosses a 1 KB TAR
boundary and (2) that a canned OCD_DAP_REQUEST response decodes to the right
words with correct posted-read discard, word alignment and trim.
"""

import struct

import pytest

from tilogger_buf_transport.xds110_reader import (
    Xds110Error,
    Xds110Reader,
    _swd_cmd,
)


def _frame(payload):
    """Wrap a firmware payload in the '*' + u16 LE length header."""
    return bytes([0x2A]) + struct.pack("<H", len(payload)) + payload


def _dap_response(words, error=0):
    """A canned OCD_DAP_REQUEST response: error code then result words."""
    return _frame(struct.pack("<i", error) + struct.pack("<%dI" % len(words), *words))


class FakeLink:
    """Stand-in for _UsbLink: records writes, replays queued responses."""

    def __init__(self, responses):
        self.writes = []
        self._responses = list(responses)

    def write(self, data):
        self.writes.append(bytes(data))

    def read(self, size, timeout):
        # Each response is delivered in one packet; the reader reassembles by
        # the header length, so a single chunk is enough for these sizes.
        if not self._responses:
            raise AssertionError("reader read more responses than were queued")
        return self._responses.pop(0)

    def close(self):
        pass


def _reader(responses, csw=0x23000012):
    r = Xds110Reader(link=FakeLink(responses))
    r._csw = csw
    r._connected = True
    return r


def test_swd_cmd_bytes():
    # START + parity encoding for every register access the reader uses.
    assert _swd_cmd(False, False, 0x08) == 0x31  # DP SELECT write
    assert _swd_cmd(True, False, 0x0C) == 0x3D   # DP RDBUFF read
    assert _swd_cmd(False, False, 0x00) == 0x01  # DP ABORT write
    assert _swd_cmd(False, True, 0x00) == 0x23   # AP CSW write
    assert _swd_cmd(False, True, 0x04) == 0x0B   # AP TAR write
    assert _swd_cmd(True, True, 0x0C) == 0x1F    # AP DRW read


def test_read_request_bytes_cross_1k_boundary():
    csw = 0x23000012
    # Three words straddling 0x20001000: two in the first 1 KB block, one after.
    words = [0x11111111, 0x22222222, 0x33333333]
    # Result stream the firmware returns for the ops below: per block a stale
    # first DRW, then real words, RDBUFF draining the last.
    results = [0xDEAD0000, words[0], words[1], 0xBEEF0000, words[2]]
    reader = _reader([_dap_response(results)], csw=csw)

    got = reader.read(0x20000FF8, 12)
    assert got == struct.pack("<3I", *words)

    expected_ops = bytes(
        [0x31, 0x00, 0x00, 0x00, 0x00]      # DP SELECT = 0
        + [0x23, 0x12, 0x00, 0x00, 0x23]    # AP CSW = 0x23000012
        + [0x0B, 0xF8, 0x0F, 0x00, 0x20]    # AP TAR = 0x20000FF8
        + [0x1F, 0x1F, 0x3D]                # 2x DRW read + RDBUFF (block 0)
        + [0x0B, 0x00, 0x10, 0x00, 0x20]    # AP TAR = 0x20001000 (1 KB re-write)
        + [0x1F, 0x3D]                      # 1x DRW read + RDBUFF (block 1)
        + [0x00]                            # queue terminator
    )
    expected = bytes([0x2A]) + struct.pack("<H", 1 + len(expected_ops)) + bytes([0x3A]) + expected_ops
    assert reader._link.writes == [expected]


def test_unaligned_read_widen_and_trim():
    # Read 2 bytes at offset 1 of a word: one word fetched, middle bytes kept.
    word = 0x44332211
    results = [0x00000000, word]  # stale DRW, then RDBUFF word
    reader = _reader([_dap_response(results)])
    assert reader.read(0x20000001, 2) == b"\x22\x33"


def test_swd_wait_then_success_retries_batch():
    word = 0xABCDEF01
    reader = _reader([
        _dap_response([], error=-613),          # SWD WAIT: busy, resend
        _dap_response([0x00000000, word]),      # retry succeeds
    ])
    assert reader.read(0x20000000, 4) == struct.pack("<I", word)
    assert len(reader._link.writes) == 2  # original batch resent once


def test_dap_error_raises():
    reader = _reader([_dap_response([], error=-1)])
    with pytest.raises(Xds110Error):
        reader.read(0x20000000, 4)


def test_setup_csw_reads_and_forces_transfer_bits():
    # Probe reports its enable/protection bits with no transfer size set; the
    # reader must keep those bits and OR in 32-bit single auto-increment.
    probe_csw = 0x23000040  # device-enable set, size/inc fields clear
    reader = Xds110Reader(link=FakeLink([_dap_response([0x00000000, probe_csw])]))
    reader._setup_csw()
    assert reader._csw == 0x23000052  # 0x40 kept, | 32-bit(0x2) | addrinc(0x10)


def test_csw_override_lets_a_different_mem_ap_be_targeted():
    """A caller can force a CSW for a mem-AP unlike the CC23xx/CC27xx default,
    so the native backend is not pinned to one device family."""
    reader = Xds110Reader(link=FakeLink([]), csw=0xA5A50052)
    reader._setup_csw()
    assert reader._csw == 0xA5A50052


# ---------------------------------------------------------------------------
# Session bring-up and teardown (no pre-set _connected)
# ---------------------------------------------------------------------------
def _cmd_ok(result=b""):
    """A command response with error 0 and an optional result payload."""
    return _frame(struct.pack("<i", 0) + result)


# XDS_CONNECT, XDS_VERSION (recent firmware), SWD_CONNECT, CMAPI_CONNECT, CMAPI_ACQUIRE.
_CONNECT_SEQ = [
    _cmd_ok(),
    _cmd_ok(struct.pack("<I", 0x02030011)),
    _cmd_ok(),
    _cmd_ok(),
    _cmd_ok(),
]


def test_connect_brings_up_session_then_reads():
    word = 0x12345678
    reader = Xds110Reader(link=FakeLink(_CONNECT_SEQ + [_dap_response([0x00000000, word])]))
    assert not reader._connected
    assert reader.read(0x20000000, 4) == struct.pack("<I", word)  # lazily connects
    assert reader._connected


def test_connect_rejects_old_firmware():
    reader = Xds110Reader(link=FakeLink([_cmd_ok(), _cmd_ok(struct.pack("<I", 0x02030010))]))
    with pytest.raises(Xds110Error, match="firmware"):
        reader.read(0x20000000, 4)


def test_command_error_raises():
    reader = Xds110Reader(link=FakeLink([_frame(struct.pack("<i", -5))]))  # XDS_CONNECT fails
    with pytest.raises(Xds110Error):
        reader.read(0x20000000, 4)


def test_close_after_connect_sends_teardown_and_frees_link():
    # close() best-effort executes four teardown commands then frees the link.
    reader = _reader([_cmd_ok(), _cmd_ok(), _cmd_ok(), _cmd_ok()])
    reader.close()
    assert reader._connected is False
    assert reader._link is None


def test_close_without_link_is_noop():
    Xds110Reader(link=None).close()  # nothing claimed, must not raise


def test_swd_fault_clears_sticky_then_retries():
    word = 0x0BADF00D
    reader = _reader([
        _dap_response([], error=-614),      # SWD FAULT
        _cmd_ok(),                          # _clear_sticky's ABORT write
        _dap_response([0x00000000, word]),  # retry succeeds
    ])
    assert reader.read(0x20000000, 4) == struct.pack("<I", word)


# ---------------------------------------------------------------------------
# _UsbLink bulk wrapper over an injected device
# ---------------------------------------------------------------------------
class FakeDev:
    def __init__(self):
        self.written = []

    def write(self, ep, data, timeout):
        self.written.append((ep, bytes(data), timeout))

    def read(self, ep, size, timeout):
        return b"\x01\x02\x03"


def test_usblink_write_read_close():
    from tilogger_buf_transport.xds110_reader import _UsbLink

    dev = FakeDev()
    link = _UsbLink(dev, interface=0, ep_in=0x81, ep_out=0x02)
    link.write(b"hi")
    assert dev.written[0][0] == 0x02 and dev.written[0][1] == b"hi"
    assert link.read(3, 100) == b"\x01\x02\x03"
    link.close()  # release/dispose on a non-usb object is swallowed, must not raise
