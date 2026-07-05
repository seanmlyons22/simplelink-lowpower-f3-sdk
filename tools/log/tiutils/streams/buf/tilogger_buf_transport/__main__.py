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

"""Standalone RAM-dump decoder, the buf equivalent of the raw ITM viewer:

    python -m tilogger_buf_transport ram.bin --elf app.out [--base 0x20000000]

Prints one line per decoded record without the tilogger pipeline. Use the
full 'tilogger ... buf --dump ...' command for pipeline-identical output.
"""

import argparse

from tilogger.tracedb import TraceDB

from .buf_transport import DEFAULT_INSTANCE, Buf_Transport
from .memory import DumpReader


def main():
    parser = argparse.ArgumentParser(prog="tilogger_buf_transport", description="Decode a LogSinkBuf RAM dump")
    parser.add_argument("dump", help="RAM dump file (.bin)")
    parser.add_argument("--elf", action="append", required=True, help="Symbol file (.out); repeatable")
    parser.add_argument("--base", default="0x20000000", help="Load address of the dump (default 0x20000000)")
    parser.add_argument("--instance", default=DEFAULT_INSTANCE, help="LogSinkBuf instance name")
    args = parser.parse_args()

    db = TraceDB(args.elf, repickle=False)
    reader = DumpReader(args.dump, int(args.base, 0))
    transport = Buf_Transport(reader, db, "buf", instance=args.instance, one_shot=True)
    # logger=None prints decoded records directly, like the raw ITM viewer.
    transport.start(logger=None)


if __name__ == "__main__":
    main()
