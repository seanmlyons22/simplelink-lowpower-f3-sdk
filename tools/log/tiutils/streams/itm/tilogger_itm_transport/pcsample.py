"""
Copyright (C) 2024, Texas Instruments Incorporated

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

"""DWT PC-sample profile export and viewing.

The device emits periodic PC samples, not call stacks, so the profile is a
flat histogram: one single-frame "stack" per function, weighted by sample
count. It is written in the speedscope JSON format and viewed in the vendored
speedscope app (speedscope/, MIT, https://github.com/jlfwong/speedscope),
which is a fully offline static HTML/JS bundle: no server, no network, one
code path on Windows/macOS/Linux.

The open mechanism mirrors speedscope's own CLI: write the profile as a
base64 loader .js next to a redirect page and open index.html with a
#localProfilePath= fragment. The redirect page exists because the macOS
"open" and Windows "start" launchers drop URL fragments; going through it
unconditionally keeps a single portable code path (webbrowser + pathlib
as_uri; no per-OS shell-outs).
"""

import base64
import json
import tempfile
import webbrowser
from collections import Counter
from pathlib import Path
from typing import Optional

SPEEDSCOPE_DIR = Path(__file__).parent / "speedscope"


def build_speedscope_profile(histogram: Counter, name: str = "ITM PC samples") -> dict:
    """Flat sampled profile in the speedscope file format.

    histogram keys are (function, file, line) tuples as collected by the
    packetiser; file/line may be None for unresolved addresses and sleep.
    Entries are sorted by weight (then name) so output is deterministic.
    """
    entries = sorted(histogram.items(), key=lambda kv: (-kv[1], kv[0][0]))
    frames = []
    samples = []
    weights = []
    for idx, ((func, file, line), count) in enumerate(entries):
        frame = {"name": func}
        if file is not None:
            frame["file"] = file
        if line is not None:
            frame["line"] = line
        frames.append(frame)
        samples.append([idx])
        weights.append(count)

    total = sum(weights)
    return {
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "name": name,
        "exporter": "tilogger-itm",
        "shared": {"frames": frames},
        "profiles": [
            {
                "type": "sampled",
                "name": name,
                # Weights are sample counts, not time.
                "unit": "none",
                "startValue": 0,
                "endValue": total,
                "samples": samples,
                "weights": weights,
            }
        ],
    }


def write_speedscope_profile(histogram: Counter, out_path: Path, name: str = "ITM PC samples") -> Path:
    out_path = Path(out_path)
    out_path.write_text(json.dumps(build_speedscope_profile(histogram, name), indent=1), encoding="utf-8")
    return out_path


def speedscope_url(profile_path: Path, tmp_dir: Optional[Path] = None) -> str:
    """Build the file:// URL that opens profile_path in the vendored viewer.

    Writes the loader .js and the fragment-preserving redirect page into
    tmp_dir (a fresh temp dir by default) and returns the redirect page URI.
    """
    index = (SPEEDSCOPE_DIR / "index.html").resolve()
    if tmp_dir is None:
        tmp_dir = Path(tempfile.mkdtemp(prefix="tilogger-pcsample-"))

    payload = base64.b64encode(Path(profile_path).read_bytes()).decode("ascii")
    js_path = tmp_dir / "profile.js"
    # The app injects <script src=localProfilePath>; the script hands the
    # profile over. Same contract as speedscope's bundled CLI.
    js_path.write_text(
        f"speedscope.loadFileFromBase64({json.dumps(Path(profile_path).name)}, {json.dumps(payload)})",
        encoding="utf-8",
    )

    viewer_url = index.as_uri() + "#localProfilePath=" + str(js_path)
    redirect = tmp_dir / "open-profile.html"
    redirect.write_text(f"<script>window.location={json.dumps(viewer_url)}</script>", encoding="utf-8")
    return redirect.resolve().as_uri()


def open_in_speedscope(profile_path: Path) -> str:
    """Open the profile in the default browser; returns the opened URI."""
    uri = speedscope_url(profile_path)
    # webbrowser picks the right launcher per OS; never shell out directly.
    webbrowser.open(uri)
    return uri
