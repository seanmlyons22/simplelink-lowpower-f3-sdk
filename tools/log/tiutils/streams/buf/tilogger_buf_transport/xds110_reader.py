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

"""Native XDS110 RAM reader over pyusb, no external debug tool needed.

Same job as PyocdReader: background AHB-AP reads over SWD without halting the
core, so the target keeps running while the host reads its RAM. The XDS110
firmware has no "read memory" command, so a block read is built the debug-port
way (set up the mem-AP, then pull words out of DRW) and shipped as one
marshalled OCD_DAP_REQUEST per USB round trip. Only what a read-only attach
needs is here and nothing else: no reset, no halt, no JTAG, no write path.

pyusb is imported lazily at connect time so the plugin still loads (and dump
replay still works) on a host without USB access, and connect errors surface
in the poll loop, which retries.
"""

import struct

from .memory import MemoryReader

# The three known XDS110 USB layouts: (vid, pid, interface, ep_in, ep_out).
# Embedded probes on TI LaunchPads enumerate as 0451:bef3/bef4 on interface 2;
# the stand-alone probe is 1cbe:02a5 on interface 0. IN endpoints carry the
# 0x80 direction bit.
_CONFIGS = (
    (0x0451, 0xBEF3, 2, 0x83, 0x02),
    (0x0451, 0xBEF4, 2, 0x83, 0x02),
    (0x1CBE, 0x02A5, 0, 0x81, 0x01),
)

# Framing: every request/response is '*' + u16 LE payload length + payload.
_START = 0x2A

# Firmware payload caps. One bulk IN packet is at most _MAX_PACKET; a single
# firmware request holds at most _MAX_DATA_BLOCK marshalled bytes and returns
# at most _MAX_RESULT_QUEUE result words.
_MAX_PACKET = 1024
_MAX_DATA_BLOCK = 4096
_MAX_RESULT_QUEUE = 1024

# Firmware API opcodes (payload[0]).
_XDS_CONNECT = 0x01
_XDS_DISCONNECT = 0x02
_XDS_VERSION = 0x03
_CMAPI_CONNECT = 0x0F
_CMAPI_DISCONNECT = 0x10
_CMAPI_ACQUIRE = 0x11
_CMAPI_RELEASE = 0x12
_SWD_CONNECT = 0x17
_SWD_DISCONNECT = 0x18
_OCD_DAP_REQUEST = 0x3A

# Firmware version that first shipped OCD_DAP_REQUEST (BCD 2.3.0.11).
_OCD_FIRMWARE_VERSION = 0x02030011

# Firmware error codes returned in the first 4 bytes of every response.
_SC_ERR_NONE = 0
_SC_ERR_SWD_WAIT = -613
_SC_ERR_SWD_FAULT = -614

# DAP register byte offsets, as used to build the SWD command byte.
_DP_ABORT = 0x00
_DP_SELECT = 0x08
_DP_RDBUFF = 0x0C
_AP_CSW = 0x00
_AP_TAR = 0x04
_AP_DRW = 0x0C

# mem-AP CSW fields. We keep whatever protection/enable bits the probe already
# set up and only force 32-bit single-auto-increment transfers.
_CSW_SIZE_MASK = 0x7
_CSW_ADDRINC_MASK = 0x3 << 4
_CSW_32BIT = 0x2
_CSW_ADDRINC_SINGLE = 0x1 << 4
# Fallback CSW if the probe's own value can't be read: AHB privileged +
# master-debug + debug-sw-enable, 32-bit single increment.
_CSW_DEFAULT = 0xA2000000 | _CSW_32BIT | _CSW_ADDRINC_SINGLE

# TAR auto-increment is only guaranteed across a 1 KB window (the counter is
# 10 bits), so TAR is re-written at every 1 KB boundary of a run.
_TAR_BLOCK = 1024

# DP ABORT value that clears every sticky error flag (STKCMP/STKERR/WDERR/ORUN).
_ABORT_CLEAR = 0x1E

# Words per OCD_DAP_REQUEST. Each word costs one result slot plus a per-block
# discard/RDBUFF slot; 1000 keeps result_count and request bytes under the
# firmware caps no matter how the run splits across 1 KB blocks.
_BATCH_WORDS = 1000

# One firmware round trip should never take this long; a stuck read fails fast
# instead of hanging the poll loop.
_TIMEOUT_MS = 4000

# Retries for a batch the target NAKs (SWD WAIT) or faults on. WAIT just means
# "busy, ask again"; FAULT needs the sticky flags cleared first.
_DAP_ATTEMPTS = 4


class Xds110Error(RuntimeError):
    """XDS110 firmware or USB transport failure."""


def _swd_cmd(is_read, is_ap, reg):
    """Build the marshalled SWD command byte for one DAP register access.

    Bits: START | APnDP | RnW | A[3:2] | even-parity(APnDP..A[3:2]). START is
    always set so a zero byte can terminate the request buffer.
    """
    cmd = (0x02 if is_ap else 0) | (0x04 if is_read else 0) | ((reg & 0x0C) << 1)
    if bin(cmd).count("1") & 1:
        cmd |= 0x20
    return cmd | 0x01


def _marshal(ops):
    """Flatten (is_read, is_ap, reg, value) ops into the firmware's request buffer.

    A read is one command byte; a write is the byte plus its 32-bit LE value.
    A trailing zero terminates the queue.
    """
    buf = bytearray()
    for is_read, is_ap, reg, value in ops:
        buf.append(_swd_cmd(is_read, is_ap, reg))
        if not is_read:
            buf += struct.pack("<I", value & 0xFFFFFFFF)
    buf.append(0)
    return bytes(buf)


def _build_read_ops(csw, addr, nwords):
    """DAP ops for reading nwords 32-bit words starting at word-aligned addr.

    Returns (ops, dest). dest has one entry per read op giving the target word
    index, or None for a discarded result. AP reads are posted: reading
    DRW returns the *previous* access, so the first DRW read after each TAR
    write is stale (discarded) and a final RDBUFF read drains the last word.
    The run is split at 1 KB boundaries because TAR only auto-increments within
    that window.
    """
    ops = [
        (False, False, _DP_SELECT, 0),  # AP 0, bank 0
        (False, True, _AP_CSW, csw),
    ]
    dest = []
    word = 0
    remaining = nwords
    while remaining:
        block = (_TAR_BLOCK - (addr & (_TAR_BLOCK - 1))) // 4
        n = min(remaining, block)
        ops.append((False, True, _AP_TAR, addr))
        for _ in range(n):
            ops.append((True, True, _AP_DRW, 0))
        ops.append((True, False, _DP_RDBUFF, 0))
        # First DRW read is the stale posted value; the next n-1 DRW reads give
        # words 0..n-2 of this block and RDBUFF gives word n-1.
        dest.append(None)
        dest.extend(word + j for j in range(n - 1))
        dest.append(word + n - 1)
        word += n
        addr += n * 4
        remaining -= n
    return ops, dest


class _UsbLink:
    """Thin bulk-transfer wrapper over one claimed XDS110 interface."""

    def __init__(self, dev, interface, ep_in, ep_out):
        self._dev = dev
        self._interface = interface
        self._ep_in = ep_in
        self._ep_out = ep_out

    def write(self, data):
        self._dev.write(self._ep_out, data, _TIMEOUT_MS)

    def read(self, size, timeout):
        return bytes(self._dev.read(self._ep_in, size, timeout))

    def close(self):
        import usb.util

        # Free the interface and handle even if the probe is already gone, so
        # the next open starts clean instead of tripping over a stale claim.
        try:
            usb.util.release_interface(self._dev, self._interface)
        except Exception:
            pass
        try:
            usb.util.dispose_resources(self._dev)
        except Exception:
            pass


def _open_usb(serial=None):
    """Find, claim and return a _UsbLink for the first matching XDS110."""
    import libusb_package
    import usb.core
    import usb.util

    for vid, pid, interface, ep_in, ep_out in _CONFIGS:
        for dev in libusb_package.find(find_all=True, idVendor=vid, idProduct=pid):
            if serial is not None:
                try:
                    if usb.util.get_string(dev, dev.iSerialNumber) != serial:
                        continue
                except usb.core.USBError:
                    continue
            # Let libusb take the interface back from any kernel driver.
            try:
                if dev.is_kernel_driver_active(interface):
                    dev.detach_kernel_driver(interface)
            except (NotImplementedError, usb.core.USBError):
                pass
            # Bulk transfers need an active configuration; leave the probe's
            # current one alone if it already has one.
            try:
                dev.get_active_configuration()
            except usb.core.USBError:
                dev.set_configuration()
            usb.util.claim_interface(dev, interface)
            return _UsbLink(dev, interface, ep_in, ep_out)
    raise Xds110Error("no XDS110 probe found (is another debugger holding it?)")


class Xds110Reader(MemoryReader):
    """Read target RAM through an XDS110 over SWD, attach-only, no core halt.

    Connection is deferred to the first read so CLI parsing stays fast and
    connect errors surface in the retrying poll loop. Pass a link for tests to
    bypass USB discovery; pass csw to skip the probe's CSW read-back and force
    a value.
    """

    retryable = True

    def __init__(self, serial=None, csw=None, link=None):
        self._serial = serial
        self._csw_override = csw
        self._link = link
        self._csw = None
        self._firmware = 0
        self._connected = False

    def read(self, addr, size):
        if not self._connected:
            self._connect()
        # Word reads are 4x fewer SWD transactions than byte reads; widen the
        # window to word alignment and trim the ends.
        start = addr & ~3
        end = (addr + size + 3) & ~3
        words = self._read_words(start, (end - start) // 4)
        data = struct.pack("<%dI" % len(words), *words)
        return data[addr - start : addr - start + size]

    def close(self):
        if self._link is None:
            return
        # Best-effort teardown mirroring the driver's quit order, then always
        # free the USB resources so a reconnect isn't wedged by a stale claim.
        if self._connected:
            for opcode in (_CMAPI_RELEASE, _CMAPI_DISCONNECT, _SWD_DISCONNECT, _XDS_DISCONNECT):
                try:
                    self._execute(opcode)
                except Exception:
                    pass
        self._connected = False
        try:
            self._link.close()
        except Exception:
            pass
        self._link = None

    # -- session bring-up --------------------------------------------------

    def _connect(self):
        if self._link is None:
            self._link = _open_usb(self._serial)
        self._command(_XDS_CONNECT)
        version = self._command(_XDS_VERSION)
        self._firmware = struct.unpack_from("<I", version, 0)[0]
        if self._firmware < _OCD_FIRMWARE_VERSION:
            raise Xds110Error(
                "XDS110 firmware %08x too old for batched DAP reads" % self._firmware
            )
        self._command(_SWD_CONNECT)
        self._command(_CMAPI_CONNECT)  # returns the DAP IDCODE, unused here
        self._command(_CMAPI_ACQUIRE)
        self._setup_csw()
        self._connected = True

    def _setup_csw(self):
        """Adopt the probe's own CSW, forcing only 32-bit auto-increment."""
        if self._csw_override is not None:
            self._csw = self._csw_override
            return
        ops = [
            (False, False, _DP_SELECT, 0),
            (True, True, _AP_CSW, 0),
            (True, False, _DP_RDBUFF, 0),
        ]
        current = self._dap_request(ops, 2)[1]
        if current in (0, 0xFFFFFFFF):
            current = _CSW_DEFAULT
        self._csw = (current & ~(_CSW_SIZE_MASK | _CSW_ADDRINC_MASK)) | _CSW_32BIT | _CSW_ADDRINC_SINGLE

    # -- read path ---------------------------------------------------------

    def _read_words(self, start, nwords):
        words = [0] * nwords
        base = 0
        while base < nwords:
            n = min(_BATCH_WORDS, nwords - base)
            ops, dest = _build_read_ops(self._csw, start + base * 4, n)
            results = self._dap_request(ops, len(dest))
            for ri, wi in enumerate(dest):
                if wi is not None:
                    words[base + wi] = results[ri]
            base += n
        return words

    def _dap_request(self, ops, result_count):
        payload = _marshal(ops)
        error = _SC_ERR_NONE
        for _ in range(_DAP_ATTEMPTS):
            error, result = self._execute(_OCD_DAP_REQUEST, payload)
            if error == _SC_ERR_NONE:
                if len(result) < result_count * 4:
                    raise Xds110Error("DAP response short: %d/%d words" %
                                      (len(result) // 4, result_count))
                return list(struct.unpack_from("<%dI" % result_count, result, 0))
            if error == _SC_ERR_SWD_WAIT:
                continue  # target busy, resend the whole batch
            if error == _SC_ERR_SWD_FAULT:
                self._clear_sticky()
                continue
            raise Xds110Error("DAP request failed: error %d" % error)
        raise Xds110Error("DAP request failed after %d attempts (error %d)" %
                          (_DAP_ATTEMPTS, error))

    def _clear_sticky(self):
        try:
            self._execute(_OCD_DAP_REQUEST, _marshal([(False, False, _DP_ABORT, _ABORT_CLEAR)]))
        except Exception:
            pass

    # -- firmware transport ------------------------------------------------

    def _command(self, opcode, payload=b""):
        error, result = self._execute(opcode, payload)
        if error != _SC_ERR_NONE:
            raise Xds110Error("command 0x%02x failed: error %d" % (opcode, error))
        return result

    def _execute(self, opcode, payload=b""):
        body = bytes([opcode]) + payload
        self._link.write(bytes([_START]) + struct.pack("<H", len(body)) + body)
        resp = self._recv(_TIMEOUT_MS)
        if len(resp) < 4:
            raise Xds110Error("response too short (%d bytes)" % len(resp))
        error = struct.unpack_from("<i", resp, 0)[0]
        return error, resp[4:]

    def _recv(self, timeout):
        """Reassemble one '*'-framed response across as many IN packets as it takes."""
        data = bytearray()
        size = None
        while size is None or len(data) < size:
            chunk = self._link.read(_MAX_PACKET, timeout)
            if size is None:
                if len(chunk) < 3 or chunk[0] != _START:
                    raise Xds110Error("bad response header")
                size = chunk[1] | (chunk[2] << 8)
                data += chunk[3:]
            else:
                data += chunk
        return bytes(data[:size])
