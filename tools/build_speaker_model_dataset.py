#!/usr/bin/env python3
"""Build the virtual-monitor speaker catalog from the public Spinorama API.

The script deliberately leaves unavailable specifications and curves empty.  It never
synthesizes a frequency response.  Plotly binary arrays returned by the API are decoded,
resampled to 200 logarithmic points, and normalized to their own arithmetic mean.
"""

from __future__ import annotations

import base64
import concurrent.futures
import json
import math
import re
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

API = "https://api.spinorama.org/v1"
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REQUEST = Path("/Users/hasnagm4pro/.codex/attachments/0a22ceb8-242f-4b7f-a881-3a8bde95a29b/pasted-text.txt")
DEFAULT_OUTPUT = ROOT / "resources" / "speaker_model_dataset.json"

ALIASES = {
    "Yamaha NS-10": "Yamaha NS-10M Studio",
    "Yamaha NS-10M": "Yamaha NS-10M Studio",
    "Yamaha NS-10M Pro": "Yamaha NS-10M Studio",
    "ADAM T5V": "Adam T5V", "ADAM T7V": "Adam T7V", "ADAM T8V": "Adam T8V",
    "ADAM A3X": "Adam A3X", "ADAM A4V": "Adam A4V", "ADAM A44H": "Adam A44H",
    "ADAM A5X": "Adam A5X", "ADAM A7V": "Adam A7V", "ADAM A7X": "Adam A7X",
    "ADAM A77H": "Adam A77H", "ADAM A8H": "Adam A8H", "ADAM S2V": "Adam S2V",
    "ADAM S3V": "Adam S3V", "ADAM S3H": "Adam S3H", "ADAM S5V": "Adam S5V",
    "ADAM S5H": "Adam S5H", "ADAM S6X": "Adam S6X",
    "Neumann KH 80 DSP": "Neumann KH 80", "Neumann KH 310": "Neumann KH 310A",
    "Dutch & Dutch 8c": "Dutch Dutch 8C", "Klein + Hummel O 300": "Neumann O 300",
    "JBL 305P MkII": "JBL 305P Mark ii", "JBL 306P MkII": "JBL 306P Mark ii",
    "JBL 308P MkII": "JBL 308P Mark ii", "Kali LP-6": "Kali LP-6v1",
    "Kali LP-8": "Kali LP-8v1", "Manger P1": "Manger Audio P1",
}

BRANDS = sorted({
    "Yamaha", "Auratone", "Avantone Pro", "Genelec", "Neumann", "ADAM", "Focal",
    "Dynaudio", "KRK", "JBL", "Mackie", "PreSonus", "Kali", "EVE Audio", "HEDD",
    "Amphion", "ATC", "PMC", "Barefoot", "Quested", "Ocean Way", "Augspurger",
    "Meyer Sound", "Kii", "Dutch & Dutch", "GGNTKT", "PSI Audio", "Manger",
    "Unity Audio", "Klein + Hummel", "Tannoy", "Westlake",
}, key=len, reverse=True)


def get_json(path: str):
    req = urllib.request.Request(API + path, headers={"User-Agent": "Neuracoust-dataset-builder/1"})
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def catalog_names(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    block = text.split("LIST (184 models", 1)[1]
    names = [line.strip().rstrip(".") for line in block.splitlines() if re.search(r"\((?:NF|MF|LF)\)$", line.strip().rstrip("."))]
    if len(names) != 184 or len(names) != len(set(names)):
        raise ValueError(f"expected 184 unique catalog names, found {len(names)}")
    return names


def clean_name(catalog_name: str) -> tuple[str, str]:
    match = re.fullmatch(r"(.+) \((NF|MF|LF)\)", catalog_name)
    if not match:
        raise ValueError(catalog_name)
    return match.group(1), match.group(2)


def normalized(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.casefold().replace("markii", "mkii"))


def match_speaker(clean: str, speakers: list[str]) -> str | None:
    if clean in ALIASES and ALIASES[clean] in speakers:
        return ALIASES[clean]
    lookup = {normalized(item): item for item in speakers}
    if normalized(clean) in lookup:
        return lookup[normalized(clean)]
    return None


def catalog_identity(clean: str) -> tuple[str, str]:
    brand = next((item for item in BRANDS if clean.casefold().startswith(item.casefold() + " ")), clean.split()[0])
    return brand, clean[len(brand):].strip()


def choose_measurement(metadata: dict) -> tuple[str, dict] | tuple[None, None]:
    measurements = metadata.get("measurements", {})
    def rank(item):
        key, data = item
        origin = str(data.get("origin", "")).casefold()
        fmt = str(data.get("format", "")).casefold()
        independent = any(tag in origin for tag in ("asr", "erin", "napilopez", "princeton", "klippel"))
        return (3 if independent and fmt == "klippel" else 2 if independent else 1 if key == "vendor" or "vendor" in origin else 0,
                1 if data.get("quality") == "high" else 0,
                1 if key == metadata.get("default_measurement") else 0)
    valid = [(k, v) for k, v in measurements.items() if isinstance(v, dict)]
    return max(valid, key=rank) if valid else (None, None)


def unpack_array(value) -> list[float]:
    if isinstance(value, list):
        return [float(v) for v in value]
    if not isinstance(value, dict) or "bdata" not in value:
        return []
    dtype = value.get("dtype", "f8")
    formats = {"f8": "d", "f4": "f", "i4": "i", "i2": "h", "i1": "b", "u4": "I", "u2": "H", "u1": "B"}
    code = formats.get(dtype)
    if not code:
        return []
    raw = base64.b64decode(value["bdata"])
    size = struct.calcsize("<" + code)
    return [float(x[0]) for x in struct.iter_unpack("<" + code, raw[:len(raw) // size * size])]


def fetch_curve(speaker: str, version: str) -> tuple[list[list[float]] | None, str | None]:
    quoted_speaker = urllib.parse.quote(speaker, safe="")
    quoted_version = urllib.parse.quote(version, safe="")
    measurements = get_json(f"/speaker/{quoted_speaker}/version/{quoted_version}/measurements")
    endpoint = "CEA2034" if "CEA2034" in measurements else "On Axis" if "On Axis" in measurements else None
    if not endpoint:
        return None, None
    path = f"/speaker/{quoted_speaker}/version/{quoted_version}/measurements/{urllib.parse.quote(endpoint, safe='')}"
    payload = get_json(path)
    # API response is currently [plotly-json-string], but accept a direct object too.
    if isinstance(payload, list) and payload and isinstance(payload[0], str):
        plot = json.loads(payload[0])
    elif isinstance(payload, str):
        plot = json.loads(payload)
    else:
        plot = payload
    traces = plot.get("data", []) if isinstance(plot, dict) else []
    trace = next((t for t in traces if t.get("name") == "Listening Window"), None)
    if trace is None:
        trace = next((t for t in traces if t.get("name") == "On Axis"), None)
    if trace is None:
        return None, None
    xs, ys = unpack_array(trace.get("x")), unpack_array(trace.get("y"))
    pairs = sorted((x, y) for x, y in zip(xs, ys) if 20 <= x <= 20000 and math.isfinite(x) and math.isfinite(y))
    if len(pairs) < 2:
        return None, None
    targets = [20 * (1000 ** (i / 199)) for i in range(200)]
    out, cursor = [], 0
    for target in targets:
        while cursor + 1 < len(pairs) and pairs[cursor + 1][0] < target:
            cursor += 1
        if target <= pairs[0][0]:
            value = pairs[0][1]
        elif target >= pairs[-1][0]:
            value = pairs[-1][1]
        else:
            x0, y0 = pairs[cursor]; x1, y1 = pairs[cursor + 1]
            ratio = (math.log(target) - math.log(x0)) / (math.log(x1) - math.log(x0))
            value = y0 + ratio * (y1 - y0)
        out.append([round(target, 2), value])
    mean = sum(point[1] for point in out) / len(out)
    return [[freq, round(level - mean, 2)] for freq, level in out], trace.get("name")


def tonal_summary(points: list[list[float]] | None) -> str:
    if not points:
        return "No verified measured response curve was located; tonal balance is intentionally not inferred."
    def avg(lo, hi):
        vals = [db for hz, db in points if lo <= hz <= hi]
        return sum(vals) / len(vals) if vals else 0.0
    bass, mid, treble = avg(40, 200), avg(300, 2000), avg(4000, 16000)
    return f"Measured mean-relative balance: bass {bass:+.1f} dB, midrange {mid:+.1f} dB, treble {treble:+.1f} dB over broad bands; local narrow-band features remain in the curve."


def form_factor(shape: str | None) -> str | None:
    return {"bookshelves": "bookshelf monitor", "floorstanders": "floorstanding speaker", "center": "center-channel speaker"}.get(shape or "", shape)


def build_one(catalog_name: str, speakers: list[str]) -> dict:
    clean, field = clean_name(catalog_name)
    fallback_brand, fallback_model = catalog_identity(clean)
    matched = match_speaker(clean, speakers)
    metadata, version, measurement = {}, None, None
    points = curve_name = None
    if matched:
        try:
            metadata = get_json(f"/speaker/{urllib.parse.quote(matched, safe='')}/metadata")
            version, measurement = choose_measurement(metadata)
            if version:
                points, curve_name = fetch_curve(matched, version)
        except (urllib.error.URLError, ValueError, KeyError, json.JSONDecodeError):
            pass
    origin = str((measurement or {}).get("origin", ""))
    fmt = str((measurement or {}).get("format", ""))
    is_vendor = version == "vendor" or "vendor" in origin.casefold()
    confidence = "datasheet" if points and is_vendor else "measured" if points else "estimated"
    specs = (measurement or {}).get("specifications", {})
    reviews = list(((measurement or {}).get("reviews") or {}).values())
    source_url = None
    if points and matched and version:
        source_url = f"https://api.spinorama.org/v1/speaker/{urllib.parse.quote(matched, safe='')}/version/{urllib.parse.quote(version, safe='')}/measurements/{urllib.parse.quote('CEA2034' if curve_name == 'Listening Window' else 'On Axis', safe='')}"
    sensitivity = specs.get("sensitivity")
    impedance = specs.get("impedance") if metadata.get("type") == "passive" else None
    shared = "Yamaha NS-10M Studio (NF)" if clean in {"Yamaha NS-10", "Yamaha NS-10M", "Yamaha NS-10M Pro"} and points else None
    notes = []
    if matched and matched != clean:
        notes.append(f"Spinorama match: {matched}.")
    if shared:
        notes.append("Catalog variant shares the NS-10M Studio measurement by explicit family mapping; it is not claimed to be an independently measured sample.")
    if not matched:
        notes.append("No conservative exact/alias Spinorama match was found; response curve left null.")
    elif not points:
        notes.append("A catalog match exists, but no parseable Listening Window or On-Axis curve was returned; response curve left null.")
    sources = list(dict.fromkeys(([source_url] if source_url else []) + [u for u in reviews if isinstance(u, str)]))
    return {
        "catalog_name": catalog_name,
        "brand": metadata.get("brand") or fallback_brand,
        "model": metadata.get("model") or fallback_model,
        "type": metadata.get("type") if metadata.get("type") in {"active", "passive"} else None,
        "field": field,
        "form_factor": form_factor(metadata.get("shape")),
        "drivers": None,
        "enclosure": None,
        "freq_response_hz": {"low": None, "high": None, "tolerance_db": None},
        "sensitivity_db": sensitivity if isinstance(sensitivity, (int, float)) else None,
        "impedance_ohm": impedance if isinstance(impedance, (int, float)) else None,
        "amplification": "passive (external amp)" if metadata.get("type") == "passive" else "active (power breakdown unavailable)" if metadata.get("type") == "active" else None,
        "crossover_hz": [],
        "tonal_signature": tonal_summary(points),
        "response_curve": {"confidence": confidence, "source": f"Spinorama {curve_name}" if points else "estimated-from-class", "source_url": source_url, "points": points},
        "shared_curve_with": shared,
        "sources": sources,
        "notes": " ".join(notes),
    }


def main() -> int:
    request = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_REQUEST
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT
    names = catalog_names(request)
    speakers = get_json("/speakers")
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        rows = list(pool.map(lambda name: build_one(name, speakers), names))
    output.write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    measured = sum(row["response_curve"]["points"] is not None for row in rows)
    print(f"wrote {len(rows)} models ({measured} with curves) to {output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
