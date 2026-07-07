"""PC-sample profile generator and viewer-open-path tests. The emitted JSON
is pinned against a known sample set so a format drift is caught without a
browser."""

import json
from collections import Counter

from tilogger_itm_transport import pcsample


HIST = Counter(
    {
        ("uart_isr", "uart.c", 88): 420,
        ("main_loop", "main.c", 12): 300,
        ("<sleep>", None, None): 700,
        ("0x08001234", None, None): 25,
    }
)

EXPECTED_PROFILE = {
    "$schema": "https://www.speedscope.app/file-format-schema.json",
    "name": "spike",
    "exporter": "tilogger-itm",
    "shared": {
        "frames": [
            {"name": "<sleep>"},
            {"name": "uart_isr", "file": "uart.c", "line": 88},
            {"name": "main_loop", "file": "main.c", "line": 12},
            {"name": "0x08001234"},
        ]
    },
    "profiles": [
        {
            "type": "sampled",
            "name": "spike",
            "unit": "none",
            "startValue": 0,
            "endValue": 1445,
            "samples": [[0], [1], [2], [3]],
            "weights": [700, 420, 300, 25],
        }
    ],
}


def test_profile_pinned():
    assert pcsample.build_speedscope_profile(HIST, name="spike") == EXPECTED_PROFILE


def test_write_and_reload(tmp_path):
    out = pcsample.write_speedscope_profile(HIST, tmp_path / "p.speedscope.json", name="spike")
    assert json.loads(out.read_text()) == EXPECTED_PROFILE


def test_vendored_viewer_is_packaged():
    """The offline viewer must ship with the package."""
    assert (pcsample.SPEEDSCOPE_DIR / "index.html").is_file()
    assert (pcsample.SPEEDSCOPE_DIR / "LICENSE").is_file()
    # The app itself: one hashed js bundle
    assert list(pcsample.SPEEDSCOPE_DIR.glob("speedscope-*.js"))


def test_speedscope_url_is_portable(tmp_path):
    """Open path is built from resolved file URIs (drive letters and spaces
    survive) and goes through the fragment-preserving redirect page."""
    prof = pcsample.write_speedscope_profile(HIST, tmp_path / "p.speedscope.json")
    uri = pcsample.speedscope_url(prof, tmp_dir=tmp_path)

    assert uri.startswith("file://")
    assert uri.endswith("open-profile.html")

    redirect = tmp_path / "open-profile.html"
    target = redirect.read_text()
    assert "index.html#localProfilePath=" in target
    assert str(tmp_path / "profile.js") in target

    loader = (tmp_path / "profile.js").read_text()
    assert loader.startswith('speedscope.loadFileFromBase64("p.speedscope.json"')


def test_open_uses_webbrowser(tmp_path, monkeypatch):
    opened = []
    monkeypatch.setattr(pcsample.webbrowser, "open", opened.append)
    prof = pcsample.write_speedscope_profile(HIST, tmp_path / "p.speedscope.json")
    uri = pcsample.open_in_speedscope(prof)
    assert opened == [uri]
    assert uri.startswith("file://")
