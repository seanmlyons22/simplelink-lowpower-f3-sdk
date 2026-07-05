"""
Copyright (C) 2026, Texas Instruments Incorporated

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

"""Memory backends for the LogSinkBuf transport.

The transport only ever needs "give me size bytes at this address", so the
backend is one method. PyocdReader serves the live-probe path, DumpReader
serves RAM-dump replay and all of the tests. Both are read-only by design:
the sink is built to run with no host attached, so the host never writes the
read pointer (or anything else) back.
"""

import abc
import struct
from pathlib import Path


class MemoryReader(abc.ABC):
    """Read-only window into target memory."""

    # Whether a failed read is worth retrying (live probe glitch) or is
    # final (dump out of range, test bug).
    retryable = False

    @abc.abstractmethod
    def read(self, addr: int, size: int) -> bytes:
        """Return exactly size bytes at addr."""

    def close(self) -> None:
        pass


class PyocdReader(MemoryReader):
    """Background AHB-AP reads over SWD via pyOCD, without halting the core.

    pyOCD is an optional extra (pip install -e streams/buf[probe]); the import
    is deferred to connect time so the plugin loads and dump replay works
    without it. The connection itself is also deferred to the first read so
    CLI parsing stays fast and connect errors surface in the poll loop, which
    retries.
    """

    retryable = True

    def __init__(self, probe=None, target: str = "cortex_m", frequency=None,
                 limit_packets: int = 1, prefer_v1: bool = False):
        self._probe = probe
        self._target_type = target
        self._frequency = frequency
        self._limit_packets = limit_packets
        self._prefer_v1 = prefer_v1
        self._session = None
        self._target = None

    def _connect(self):
        try:
            from pyocd.core.helpers import ConnectHelper
        except ImportError as exc:
            raise RuntimeError(
                "pyOCD is not installed; install the probe extra: pip install -e streams/buf[probe]"
            ) from exc

        # attach mode: never halt or reset, the target keeps running while we
        # read RAM behind its back.
        options = {"target_override": self._target_type, "connect_mode": "attach"}
        # The XDS110 firmware (SWD backchannel on TI LaunchPads) only services one
        # outstanding CMSIS-DAP command at a time; pyOCD's default of several in
        # flight times out mid-read and leaves the USB interface claimed, so every
        # later reconnect fails until the probe is physically re-plugged. Serialising
        # to one packet keeps the link stable. Harmless (just slower) on probes that
        # do support pipelining.
        if self._limit_packets:
            options["cmsis_dap.limit_packets"] = self._limit_packets
        if self._prefer_v1:
            options["cmsis_dap.prefer_v1"] = True
        if self._frequency:
            options["frequency"] = self._frequency
        session = ConnectHelper.session_with_chosen_probe(unique_id=self._probe, options=options)
        if session is None:
            raise RuntimeError("no debug probe found (or selection cancelled)")
        # Track the session before open() so a failed open() is still torn down by
        # close() on the next retry; a leaked half-open session keeps the interface
        # claimed and wedges every subsequent reconnect.
        self._session = session
        session.open()
        self._target = session.target

    def read(self, addr: int, size: int) -> bytes:
        if self._target is None:
            self._connect()
        # Word reads are 4x fewer SWD transactions than byte reads; widen the
        # window to word alignment and trim the ends.
        start = addr & ~3
        end = (addr + size + 3) & ~3
        words = self._target.read_memory_block32(start, (end - start) // 4)
        data = struct.pack("<%dI" % len(words), *words)
        return data[addr - start : addr - start + size]

    def close(self) -> None:
        session, self._session, self._target = self._session, None, None
        if session is not None:
            # Always drop our references even if the probe errors on disconnect,
            # so the next reconnect starts from a clean slate instead of reusing a
            # dead session.
            try:
                session.close()
            except Exception:
                pass


class DumpReader(MemoryReader):
    """Decode from a saved RAM dump instead of a live target.

    A dump is a static snapshot, so the transport runs exactly one decode
    pass over it and exits instead of polling.
    """

    def __init__(self, path, base: int):
        self._path = Path(path)
        self._data = self._path.read_bytes()
        self._base = base

    def read(self, addr: int, size: int) -> bytes:
        off = addr - self._base
        if off < 0 or off + size > len(self._data):
            raise ValueError(
                "read [0x%08x, +%d) is outside the dump %s [0x%08x, +%d); "
                "wrong --base or the dump does not cover the sink"
                % (addr, size, self._path.name, self._base, len(self._data))
            )
        return self._data[off : off + size]
