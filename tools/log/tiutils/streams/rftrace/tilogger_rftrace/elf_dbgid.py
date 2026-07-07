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

"""Extract the RF-tracer dbgid table directly from an application ELF (.out).

Ports the SDK `elf2dbgid extract` transform onto tilogger's own `TraceDB` (which already
parses the ELF via pyelftools and exposes the identical `stringpointers` / `traceDB`
interface elf2dbgid's `lib.elfstrings.TraceDB` uses). So `tilogger rftrace --elf app.out`
resolves every CPU-side log with no manual elf2dbgid step - the metadata is exactly what
appears on the wire.

The 32-bit log pointer encodes channel (bits 27:24) and dbgid (bits 15:2); the dbgid the
decoder sees on the wire == `pointer_to_dbg` below. Extra `--dbgid` files (modem/LRF:
pbe/rfe/mce) are layered on top by the transport.

Only stdlib at module top so the pure transform + self-check run without pyelftools;
`TraceDB` is imported lazily inside `elf_to_dbgid_text`.
"""

import os
import tempfile


def pointer_to_dbg(pointer):
    """(channel 1..3, dbgid) for a log pointer, or None if out of range.

    Verbatim from elf2dbgid.extract: channel-1 log pointers were relocated to 0x94.. (so the
    channel field reads as 4), and their dbgid is therefore off by one (+1). Channels 2/3 are
    unmoved. dbgid 1/2 are reserved (the Rust decoder drops dbgid < 3).
    """
    dbgch = (pointer >> 24) & 0x0F
    if dbgch == 4:
        dbgch = 1
    if dbgch == 1:
        dbgid = ((pointer & 0xFFFF) >> 2) + 1
    else:
        dbgid = ((pointer & 0xFFFF) >> 2)
    if 1 <= dbgch <= 3 and dbgid <= 255:
        return dbgch, dbgid
    return None


def _nargs_field(nargs):
    """DBG_DEF argCount: 0/1/2 -> -n (32-bit args); 3/4 -> +n (16-bit args, capped 4).
    Verbatim from elf2dbgid (without the pre-unified `--support-short-kw` path)."""
    if nargs > 2:
        return min(nargs, 4)
    return -nargs


def _dbg_def_line(pointer, entry):
    dbg = pointer_to_dbg(pointer)
    if dbg is None or entry.string is None:  # entry.string None => an Event, not a log
        return None
    dbgch, dbgid = dbg
    filesym = os.path.split(entry.file)[1].replace(".", "_")
    return (
        f'DBG_DEF(DBGID_{filesym}_{entry.line}, {dbgid}, DBGCH{dbgch}, '
        f'{_nargs_field(entry.nargs)}, "{entry.string}", "{entry.file}", {entry.line})\n'
    )


def elf_to_dbgid_text(elf_path):
    """Parse the ELF and return its DBG_DEF table as text (same format elf2dbgid emits)."""
    from tilogger.tracedb import TraceDB

    # repickle=True forces a full parse_elf: tilogger only persists _traceDB in its pickle, not
    # stringpointers, so the cached path yields no pointers. Matches elf2dbgid's own `TraceDB(_, True)`.
    db = TraceDB([str(elf_path)], True)
    try:
        # tilogger's TraceDB.stringpointers is {log_data_addr: pointer_symbol_addr} - the
        # reverse of elf2dbgid's own TraceDB. The channel+dbgid come from the pointer-symbol
        # address (the 0x9X.. value, channel-encoded); the string is at the log_data key.
        lines = []
        for log_data_addr, ptr_sym_addr in db.stringpointers.items():
            entry = db.traceDB[log_data_addr]
            line = _dbg_def_line(ptr_sym_addr, entry)
            if line:
                lines.append(line)
        return "".join(lines)
    finally:
        # TraceDB starts filesystem watchers; stop them so tilogger's wait_threads can exit.
        for obs in getattr(db, "change_observers", []):
            try:
                obs.stop()
            except Exception:
                pass


def elf_to_dbgid_file(elf_path):
    """Write the ELF's DBG_DEF table to a temp .h and return its path (caller unlinks)."""
    fd, path = tempfile.mkstemp(prefix="rftrace_elf_", suffix="_dbgid.h")
    with os.fdopen(fd, "w") as f:
        f.write(elf_to_dbgid_text(elf_path))
    return path


def _selfcheck():
    # channel-1 log ptr lives at 0x94.. and the dbgid is +1 (e.g. RCL.c:884 -> dbgid 58).
    assert pointer_to_dbg(0x940000E4) == (1, 58), pointer_to_dbg(0x940000E4)
    # channels 2/3 are unmoved: dbgid = (ptr & 0xFFFF) >> 2.
    assert pointer_to_dbg(0x92000200) == (2, 128), pointer_to_dbg(0x92000200)
    assert pointer_to_dbg(0x93000010) == (3, 4), pointer_to_dbg(0x93000010)
    assert pointer_to_dbg(0x90000000) is None  # channel 0 -> not a log channel
    assert (_nargs_field(0), _nargs_field(2), _nargs_field(3), _nargs_field(9)) == (0, -2, 3, 4)
    print("elf_dbgid self-check OK")


if __name__ == "__main__":
    _selfcheck()
