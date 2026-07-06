# Host tooling test coverage (buf + itm)

Measured with `pytest-cov` over the offline suites (`core/tests`,
`streams/itm/tests`, `streams/buf/tests`). HIL/bench tests stay skipped while
their env vars are unset, so these numbers are pure host-decode coverage with
no board attached.

Reproduce (per package gives stable per-file numbers; a single combined run
mis-attributes a couple of transport modules to 0% due to a pytest-cov
multi-root quirk):

```
cd tools/log/tiutils
.venv/bin/python -m pytest streams/itm/tests --cov=tilogger_itm_transport --cov-report=term-missing
.venv/bin/python -m pytest streams/buf/tests --cov=tilogger_buf_transport --cov-report=term-missing
.venv/bin/python -m pytest core/tests streams/itm/tests streams/buf/tests --cov=core/tilogger --cov-report=term-missing
```

Two throughput tests (`test_rate_spike_sustained_3mbps`,
`test_decode_faster_than_the_wire`) fail *only* under coverage: the tracer
throttles the parser below the asserted MB/s. They pass clean without `--cov`.
Deselect them when collecting coverage.

## Numbers

Decode / parse hot paths are well covered; the gaps are hardware I/O and CLI
glue that need a real board, a real ELF, or an interactive run.

| Module | Stmts | Cov | What is not covered |
|--------|------:|----:|---------------------|
| `itm_framer.py`        | 305 | 92% | multi-byte extension packet, a few reserved-header branches |
| `itm_to_log.py`        | 281 | 94% | DWT-event corner cases, some str() variants |
| `pcsample.py`          |  44 |100% | - |
| `serial_rx.py`         |  54 | 85% | live-serial open/close error paths |
| `itm_transport.py`     | 102 | 65% | CLI callback + profile-write (needs a run) |
| `core/interface.py`    |  66 | 95% | - |
| `core/bufdecode.py`    | 188 | 90% | malformed-COBS / truncated-record branches |
| `core/helpers.py`      |   6 |100% | - |
| `buf_transport.py`     | 185 | 73% | CLI `main()` + live-stream loop (lines 310-408) |
| `xds110_reader.py`     | 216 | 63% | native USB DAP protocol (needs the probe) |
| `memory.py` (buf)      |  63 | 40% | PyocdReader live path (needs the probe) |
| `core/tracedb.py`      | 266 | 32% | ELF/DWARF symbol DB load (needs a real .out) |
| `core/dwarf.py`        | 145 | 32% | DWARF parse (needs a real .out) |
| `core/logger.py`       | 109 | 45% | top-level orchestration / output routing |
| `__main__.py` (all)    |   - |  0% | module entry points |

Package totals: ITM 89%, BUF transport 61%.

## Where coverage is intentionally low

- **Probe / USB code** (`xds110_reader.py`, `memory.py` PyocdReader): exercised
  only by the HIL suites against real hardware, not offline.
- **Symbol database** (`tracedb.py`, `dwarf.py`): needs a real `.out`; covered
  when a HIL/e2e run loads one.
- **CLI entry glue** (`__main__.py`, transport callbacks, `buf_transport.main`):
  covered by end-to-end invocation, not unit tests.

The pure-logic parsers - the code most likely to break silently on bad wire
data - sit at 90%+. Remaining logic gaps (framer extension packets, bufdecode
truncation handling, xds110 TAR-wrap arithmetic) are the targets for the stress
tests, which do not need a board.
