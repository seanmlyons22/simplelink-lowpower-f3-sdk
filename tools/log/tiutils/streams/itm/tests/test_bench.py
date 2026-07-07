"""Gated wrapper for the Task 1 benchmark harness (bench_itm.py) so it can
run under pytest without slowing the ordinary suite. Correctness (record
count + rolling checksum) is asserted inside bench_itm.run on every run.

    ITM_BENCH=1 pytest tests/test_bench.py -s
    ITM_BENCH_MB=64 ITM_BENCH=1 pytest tests/test_bench.py -s
"""

import os

import pytest

import bench_itm

pytestmark = pytest.mark.skipif(
    os.environ.get("ITM_BENCH") != "1",
    reason="benchmark is opt-in: set ITM_BENCH=1 (and optionally ITM_BENCH_MB)",
)


@pytest.mark.parametrize("profile", ["dense", "mixed"])
def test_benchmark(profile):
    target_mb = float(os.environ.get("ITM_BENCH_MB", "32"))
    result = bench_itm.run(profile, target_mb)
    print(f"\n{result.report()}")
    assert result.mb_per_s >= bench_itm.BAR_MB_S, result.report()
