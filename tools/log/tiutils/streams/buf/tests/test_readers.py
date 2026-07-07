"""PyocdReader logic without a probe: a fake pyOCD target stands in for the
live AHB-AP so the word-align/trim, lazy-connect, and close paths are covered
offline. The DUT is never named here - the reader only ever asks for bytes at
an address."""

import struct

from tilogger_buf_transport.memory import DumpReader, MemoryReader, PyocdReader


class FakeTarget:
    """Mimics pyOCD's read_memory_block32 over a flat byte buffer."""

    def __init__(self, mem: bytes, base: int = 0x20000000):
        self.mem = mem
        self.base = base
        self.calls = []

    def read_memory_block32(self, addr, nwords):
        self.calls.append((addr, nwords))
        return [struct.unpack_from("<I", self.mem, addr - self.base + 4 * i)[0] for i in range(nwords)]


def test_pyocd_reader_word_aligns_and_trims():
    mem = bytes(range(64))
    r = PyocdReader()
    r._target = FakeTarget(mem)  # inject: skip the live connect

    got = r.read(0x20000003, 5)  # unaligned start, non-word length
    assert got == mem[3:8]

    (addr, nwords), = r._target.calls
    assert addr == 0x20000000  # widened down to a word boundary
    assert nwords == 2         # 2 words span bytes [3:8)


def test_pyocd_reader_connects_lazily_on_first_read(monkeypatch):
    r = PyocdReader()
    target = FakeTarget(bytes(16))

    def fake_connect():
        r._target = target

    monkeypatch.setattr(r, "_connect", fake_connect)
    r.read(0x20000000, 4)
    assert r._target is target


def test_pyocd_reader_close_is_idempotent_and_swallows_errors():
    r = PyocdReader()

    class BadSession:
        def close(self):
            raise RuntimeError("probe disconnected")

    r._session = BadSession()
    r.close()  # error on disconnect must not propagate
    assert r._session is None
    r.close()  # idempotent when already closed


def test_pyocd_reader_defaults_are_dut_neutral():
    r = PyocdReader()
    # No device name baked in - a generic cortex_m core type, retryable link.
    assert r._target_type == "cortex_m"
    assert r.retryable is True


def test_memory_reader_base_close_is_noop():
    class R(MemoryReader):
        def read(self, addr, size):
            return b""

    R().close()  # base class close() does nothing, must not raise


def test_dump_reader_reads_within_and_close(tmp_path):
    blob = bytes(range(256))
    dump = tmp_path / "ram.bin"
    dump.write_bytes(blob)
    r = DumpReader(dump, 0x20000000)
    assert r.read(0x20000010, 4) == blob[0x10:0x14]
    r.close()  # DumpReader close is a no-op but must exist
