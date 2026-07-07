# Host tooling test coverage (buf + itm + core)

Measured with `pytest-cov` over the offline suites (`core/tests`,
`streams/itm/tests`, `streams/buf/tests`). HIL/bench tests stay skipped while
their env vars are unset, so these numbers are pure host-decode coverage with
no board attached. Every substantive module is above 70%.

## Reproduce

Per-package runs give the authoritative per-file numbers:

```
cd tools/log/tiutils
.venv/bin/python -m pytest core/tests   --cov=tilogger              --cov-report=term-missing
.venv/bin/python -m pytest streams/itm/tests --cov=tilogger_itm_transport --cov-report=term-missing
.venv/bin/python -m pytest streams/buf/tests --cov=tilogger_buf_transport --cov-report=term-missing
```

`logger.py`'s dobby-format path is covered from the buf suite, so run all three
together (`--cov=tilogger`) to see its full number.

Caveats when collecting coverage:
- Two throughput tests (`test_rate_spike_sustained_3mbps`,
  `test_decode_faster_than_the_wire`) fail *only* under the tracer, which
  throttles the parser below the asserted MB/s. Deselect them.
- A single combined run under-reports `itm_transport.py` and `serial_rx.py`
  because the pty integration test exercises them on a background thread and
  coverage of threads is timing-dependent across a large multi-root session.
  Their per-suite numbers (75% / 85%) are the real ones.
- `__main__.py` `python -m` entry shims are omitted in `.coveragerc`: they are
  argparse glue over already-tested library code, exercised end-to-end.

## Numbers (per-suite)

| Module | Stmts | Cov |
|--------|------:|----:|
| core/tilogger/bufdecode.py     | 188 | 93% |
| core/tilogger/tracedb.py       | 263 | 88% |
| core/tilogger/dwarf.py         | 145 | 88% |
| core/tilogger/interface.py     |  66 | 95% |
| core/tilogger/logger.py        | 107 | 72% |
| core/tilogger/helpers.py       |   6 |100% |
| itm/itm_framer.py              | 305 | 98% |
| itm/itm_to_log.py              | 281 | 94% |
| itm/itm_transport.py           | 102 | 75% |
| itm/pcsample.py                |  44 |100% |
| itm/serial_rx.py               |  54 | 85% |
| buf/buf_transport.py           | 185 | 73% |
| buf/xds110_reader.py           | 216 | 84% |
| buf/memory.py                  |  61 | 72% |

## How the hard-to-reach code is covered

- **ELF symbol DB + DWARF** (`tracedb.py`, `dwarf.py`): a tiny ELF is built at
  test time with the host `cc` (`.log_data`/`.log_ptr` sections plus DWARF), so
  `parse_elf`, `ElfString`, and the DWARF helpers run with no device or
  cross-compiler. The fixture is a host-arch ELF, which also proves the decoder
  is architecture-neutral - it keys on sections/symbols/DWARF, never `e_machine`.
- **Probe readers** (`memory.py` PyocdReader, `xds110_reader.py`): a fake pyOCD
  target and a `FakeLink` inject the transport layer, so word-align/trim, lazy
  connect, session bring-up/teardown, and SWD-fault retry run without a probe.
- **Remaining gaps** are the live-run tails: `buf_transport` stream loop and
  `itm_transport.start()` need a probe / serial port and are exercised by the
  HIL suites, and `_open_usb` needs a real XDS110.
