# cython: language_level=3, boundscheck=False, wraparound=False
"""Cython spike: the ITMFramer inner deframe loop compiled, emitting the
same Python frame objects.

Kept so the Task 1 Python-vs-compiled decision stays reproducible (see
streams/itm/ARCHITECTURE.md for the measured numbers). Not built or shipped
by default. To reproduce:

    pip install cython setuptools
    cythonize -i tests/cyframer_spike.pyx
    python - <<PY
    import sys; sys.path.insert(0, "tests")
    import cyframer_spike, bench_itm
    bench_itm.ITMFramer = cyframer_spike.CyITMFramer
    print(bench_itm.run("mixed", 32).report())
    PY

The checksums must match the pure-Python harness output for the same size.
"""
import logging

from tilogger_itm_transport.itm_framer import (
    ITM_RESET_TOKEN,
    MAX_ITM_FRAME_SIZE,
    HDR_OVERFLOW,
    ITMSourceSWFrame,
    ITMSourceHwPcFrame,
    ITMSourceHwExceptionFrame,
    ITMSourceHwCntWrapFrame,
    ITMSourceHwTraceFrame,
    ITMTimestampFrame,
    ITMGlobalTimestampFrame,
    ITMExtensionFrame,
    ITMOverflowFrame,
    ITMSyncFrame,
    logger,
)


class CyITMFramer:
    def __init__(self, output_queue):
        self._output_queue = output_queue
        self.last_ts_counter = 0
        self._first_read = True

    def parse(self, buf):
        return _parse(self, buf)


def _parse(framer, buf: bytearray):
    cdef Py_ssize_t pos, end, p, token_at, payload_end
    cdef unsigned int header, low2, size, disc, b
    cdef unsigned long long ts, value
    cdef int shift
    cdef unsigned char[:] view

    if not buf:
        return buf

    token_at = buf.find(ITM_RESET_TOKEN)
    if token_at >= 0:
        if token_at:
            del buf[:token_at]
    elif (framer._first_read and buf[-1] == 0xBB) or buf[-1] == 0xFB:
        return buf
    elif framer._first_read:
        return bytearray()

    put = framer._output_queue.put
    debug = logger.isEnabledFor(logging.DEBUG)
    last_ts = framer.last_ts_counter

    pos = 0
    end = len(buf)
    view = buf
    if end >= 5:
        framer._first_read = False

    while end - pos >= 5:
        header = view[pos]
        pos += 1
        frame = None
        low2 = header & 0x03

        if low2:
            size = 4 if low2 == 3 else low2
            payload_end = pos + size
            if header & 0x04 == 0:
                frame = ITMSourceSWFrame(header, buf[pos:payload_end], last_ts)
            else:
                disc = header >> 3
                if disc == 0x02:
                    value = view[pos]
                    if size >= 2:
                        value |= view[pos + 1] << 8
                    if size == 4:
                        value |= (view[pos + 2] << 16) | (<unsigned long long> view[pos + 3]) << 24
                    frame = ITMSourceHwPcFrame(header, value, size, last_ts)
                elif disc == 0x01:
                    if size == 2:
                        frame = ITMSourceHwExceptionFrame(header, view[pos], view[pos + 1], last_ts)
                    else:
                        logger.error("Exception trace packet with bad size %d", size)
                elif disc == 0x00:
                    frame = ITMSourceHwCntWrapFrame(header, view[pos], last_ts)
                elif 0x08 <= disc <= 0x17:
                    value = view[pos]
                    if size >= 2:
                        value |= view[pos + 1] << 8
                    if size == 4:
                        value |= (view[pos + 2] << 16) | (<unsigned long long> view[pos + 3]) << 24
                    frame = ITMSourceHwTraceFrame(header, disc, value, size, last_ts)
                else:
                    logger.error("Reserved ITM hardware source packet 0x%02X", header)
            pos = payload_end

        elif header == 0x00:
            p = pos
            while p < end and view[p] == 0:
                p += 1
            if p >= end:
                pos -= 1
                break
            if view[p] == 0x80:
                p += 1
            frame = ITMSyncFrame(0, last_ts, p - pos)
            pos = p

        elif header == HDR_OVERFLOW:
            logger.warning("ITM Frame Overflow")
            frame = ITMOverflowFrame(header, last_ts)

        elif (header & 0x0F) == 0x00:
            if header & 0x80:
                ts = 0
                shift = 0
                p = pos
                while True:
                    b = view[p]
                    p += 1
                    ts += (<unsigned long long> (b & 0x7F)) << shift
                    shift += 7
                    if not b & 0x80 or shift >= 28:
                        break
                frame = ITMTimestampFrame(header, ts, p - pos)
                pos = p
            else:
                frame = ITMTimestampFrame(header, (header >> 4) & 0x7, 0)
            last_ts = frame.ts_counter
            framer.last_ts_counter = last_ts

        elif (header & 0x0B) == 0x08:
            if header & 0x80 == 0:
                frame = ITMExtensionFrame(header, (header >> 4) & 0x7, 0, last_ts)
            else:
                value = (header >> 4) & 0x7
                shift = 3
                p = pos
                ok = False
                while p < end:
                    b = view[p]
                    p += 1
                    value += (<unsigned long long> (b & 0x7F)) << shift
                    shift += 7
                    if not b & 0x80 or shift >= 31:
                        ok = True
                        break
                if not ok:
                    pos -= 1
                    break
                frame = ITMExtensionFrame(header, value, p - pos, last_ts)
                pos = p

        elif header == 0x94 or header == 0xB4:
            value = 0
            shift = 0
            p = pos
            incomplete = False
            while True:
                if p >= end:
                    incomplete = True
                    break
                b = view[p]
                p += 1
                value += (<unsigned long long> (b & 0x7F)) << shift
                shift += 7
                if not b & 0x80 or shift >= 49:
                    break
            if incomplete:
                pos -= 1
                break
            frame = ITMGlobalTimestampFrame(header, value, p - pos, last_ts)
            pos = p

        else:
            logger.error("Invalid ITM header 0x%02X", header)

        if frame is not None:
            if debug:
                logger.debug("%s", frame)
            put(frame)

    # Release the memoryview before resizing the bytearray, or CPython
    # refuses the resize (exported buffer).
    view = None
    del buf[:pos]
    return buf
