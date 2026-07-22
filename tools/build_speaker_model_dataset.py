#!/usr/bin/env python3
"""Build the virtual-monitor speaker catalog from the public Spinorama API.

The script deliberately leaves unavailable specifications and curves empty.  It never
synthesizes a frequency response. Plotly binary arrays returned by the API are decoded,
resampled to 200 logarithmic points, and normalized so the 300 Hz–3 kHz mean is 0 dB.
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
    "Genelec S360A": "Genelec S360", "Neumann KH 420": "Neumann KH 420G",
    "KRK Rokit 5 G4": "KRK Systems RoKit 5 G4", "Kii THREE": "Kii Audio Three",
}

BRANDS = sorted({
    "Yamaha", "Auratone", "Avantone Pro", "Genelec", "Neumann", "ADAM", "Focal",
    "Dynaudio", "KRK", "JBL", "Mackie", "PreSonus", "Kali", "EVE Audio", "HEDD",
    "Amphion", "ATC", "PMC", "Barefoot", "Quested", "Ocean Way", "Augspurger",
    "Meyer Sound", "Kii", "Dutch & Dutch", "GGNTKT", "PSI Audio", "Manger",
    "Unity Audio", "Klein + Hummel", "Tannoy", "Westlake", "Meyer Sound",
}, key=len, reverse=True)

# Explicit additions backed by independent high-quality Klippel measurements.
# Do not grow this list from vendor plots or name-only matches.
ADDITIONAL_CATALOG_MODELS = [
    "ADAM D3V (NF)",
    "Genelec M040 (NF)",
    "Kali LP-6v2 (NF)",
    "Kali LP-UNF (NF)",
    "Kali SM-5 (NF)",
    "PreSonus Eris E5 XT (NF)",
]

# Manufacturer measurement digitized from Figure 1's continuous 1/3-octave
# curve. The paper describes a 0.5 m tweeter-axis measurement with one boundary
# 8 ft away. These anchors deliberately stop at 20 kHz; no response is invented
# beyond the published graph. The source measurement is excellent, while the
# digitization step makes the production curve medium confidence.
MEYER_HD1_ANCHORS = [
    [30.0, -12.0], [31.5, -11.0], [35.0, -7.0], [40.0, -3.0],
    [50.0, 0.0], [63.0, 0.4], [80.0, 0.8], [100.0, 0.0],
    [125.0, 0.2], [160.0, 0.5], [200.0, 0.0], [250.0, 0.5],
    [315.0, 0.3], [400.0, -0.7], [500.0, -0.6], [630.0, 0.4],
    [800.0, 0.0], [1000.0, 0.7], [1250.0, 0.3], [1600.0, -0.4],
    [2000.0, 0.1], [2500.0, 0.1], [3150.0, -0.4], [4000.0, -0.4],
    [5000.0, -0.5], [6300.0, 0.0], [8000.0, -0.6], [10000.0, 0.3],
    [12500.0, 0.7], [16000.0, 0.4], [18000.0, 0.0], [20000.0, -1.0],
]

MEYER_HD1_REPORT = (
    "https://docs.meyersound.com/pdf/technical-reports/"
    "18.550.064.01%20B%20FREQ%20RESPONSE%20MEASUREMENTS%20%26%20THE%20HD-1.pdf"
)
MEYER_HD1_INDEPENDENT = "https://www.diyaudio.com/community/threads/meyer-sound-hd1-looking-inside.388266/"


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
    value = value.casefold()
    value = re.sub(r"\b(?:mark|mk)\s*(?:ii|2)\b", "mk2", value)
    value = re.sub(r"\bii\b", "2", value)
    return re.sub(r"[^a-z0-9]", "", value)


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
    """Return Spinorama's declared default measurement, never a guessed alternative."""
    measurements = metadata.get("measurements", {})
    version = metadata.get("default_measurement")
    measurement = measurements.get(version) if version else None
    return (version, measurement) if isinstance(measurement, dict) else (None, None)


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


def normalize_midband(points: list[list[float]]) -> list[list[float]]:
    mid = [level for freq, level in points if 300 <= freq <= 3000]
    if not mid:
        raise ValueError("curve has no samples between 300 Hz and 3 kHz")
    mean = sum(mid) / len(mid)
    return [[round(freq, 2), round(level - mean, 2)] for freq, level in points]


def resample_log_curve(anchors: list[list[float]], count: int = 200) -> list[list[float]]:
    pairs = sorted((float(freq), float(level)) for freq, level in anchors)
    lo, hi = pairs[0][0], pairs[-1][0]
    targets = [lo * ((hi / lo) ** (i / (count - 1))) for i in range(count)]
    out, cursor = [], 0
    for target in targets:
        while cursor + 1 < len(pairs) and pairs[cursor + 1][0] < target:
            cursor += 1
        if target <= pairs[0][0]:
            value = pairs[0][1]
        elif target >= pairs[-1][0]:
            value = pairs[-1][1]
        else:
            x0, y0 = pairs[cursor]
            x1, y1 = pairs[cursor + 1]
            ratio = (math.log(target) - math.log(x0)) / (math.log(x1) - math.log(x0))
            value = y0 + ratio * (y1 - y0)
        out.append([round(target, 2), value])
    return normalize_midband(out)


def meyer_hd1_entry() -> dict:
    points = resample_log_curve(MEYER_HD1_ANCHORS)
    return {
        "catalog_name": "Meyer Sound HD-1 (NF)",
        "brand": "Meyer Sound",
        "model": "HD-1",
        "type": "active",
        "field": "NF",
        "form_factor": "bookshelf monitor",
        "drivers": "8-inch cone woofer; 1-inch soft-dome tweeter",
        "enclosure": "vented",
        "freq_response_hz": {"low": 32.0, "high": 22000.0, "tolerance_db": 3.0},
        "sensitivity_db": None,
        "impedance_ohm": None,
        "amplification": "active (two-channel complementary MOSFET)",
        "crossover_hz": [],
        "tonal_signature": tonal_summary(points),
        "response_curve": {
            "confidence": "measured",
            "source": "Meyer Sound official continuous 1/3-octave response (digitized)",
            "source_url": MEYER_HD1_REPORT,
            "points": points,
            "normalized": True,
        },
        "shared_curve_with": None,
        "sources": [MEYER_HD1_REPORT, MEYER_HD1_INDEPENDENT],
        "research": {
            "spinorama_match": None,
            "selected_measurement": {
                "version": "meyer-figure-1-continuous-third-octave",
                "origin": "Meyer Sound",
                "method": "webplotdigitizer",
                "quality": "medium",
            },
            "measurement_evidence": [{
                "version": "official-technical-report-figure-1",
                "origin": "Meyer Sound",
                "method": "multiple FFT; continuous 1/3-octave presentation",
                "quality": "manufacturer-measured; digitized",
                "published": "2015-04",
                "selected_for_curve": True,
                "links": [MEYER_HD1_REPORT],
                "conditions": "0.5 m on tweeter axis; one boundary 8 ft from cabinet",
            }, {
                "version": "1997-independent-outdoor",
                "origin": "Jack Hidley / diyAudio",
                "method": "outdoor FFT; on-axis 1 m and 30-degree lateral",
                "quality": "supporting evidence",
                "published": "2022-07-21",
                "selected_for_curve": False,
                "links": [MEYER_HD1_INDEPENDENT],
            }],
            "raw_specifications": {
                "free_field_frequency_response": "32 Hz-22 kHz at -3 dB; 40 Hz-20 kHz +/-1 dB at 1/3-octave resolution",
                "maximum_spl": "125 dB peak; 120 dB at 1 m",
                "measurement_note": "Each production unit was individually factory calibrated.",
            },
            "metadata_url": MEYER_HD1_REPORT,
            "provenance_note": "Official measured curve digitized from Figure 1. The measurement is authoritative; point accuracy is limited by graph reading and 1/3-octave smoothing.",
        },
        "notes": "Not in Spinorama. Curve is log-interpolated from manually digitized Figure 1 anchors and must not be described as raw Meyer Sound numeric data.",
    }


def fetch_curve(speaker: str, version: str) -> tuple[list[list[float]] | None, str | None]:
    quoted_speaker = urllib.parse.quote(speaker, safe="")
    quoted_version = urllib.parse.quote(version, safe="")
    measurements = get_json(f"/speaker/{quoted_speaker}/version/{quoted_version}/measurements")
    if "CEA2034" not in measurements:
        return None, None
    path = f"/speaker/{quoted_speaker}/version/{quoted_version}/measurements/CEA2034"
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
    return normalize_midband(out), trace.get("name")


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
    confidence = "measured" if points else "estimated"
    specs = (measurement or {}).get("specifications", {})
    reviews = list(((measurement or {}).get("reviews") or {}).values())
    metadata_url = (f"https://api.spinorama.org/v1/speaker/{urllib.parse.quote(matched, safe='')}/metadata"
                    if matched else None)
    evidence = []
    all_links = []
    for key, item in metadata.get("measurements", {}).items():
        if not isinstance(item, dict):
            continue
        links = []
        website = item.get("website")
        if isinstance(website, str) and website.startswith(("http://", "https://")):
            links.append(website)
        links.extend(value for value in (item.get("reviews") or {}).values()
                     if isinstance(value, str) and value.startswith(("http://", "https://")))
        all_links.extend(links)
        evidence.append({
            "version": key,
            "origin": item.get("origin"),
            "method": item.get("format"),
            "quality": item.get("quality"),
            "published": item.get("review_published"),
            "selected_for_curve": key == version,
            "links": list(dict.fromkeys(links)),
        })
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
    sources = list(dict.fromkeys(([metadata_url] if metadata_url else []) +
                                ([source_url] if source_url else []) + all_links +
                                [u for u in reviews if isinstance(u, str)]))
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
        "response_curve": {"confidence": confidence,
                           "source": "Spinorama Listening Window" if points else "estimated-from-class",
                           "source_url": source_url, "points": points,
                           "normalized": bool(points)},
        "shared_curve_with": shared,
        "sources": sources,
        "research": {
            "spinorama_match": matched,
            "selected_measurement": {
                "version": version,
                "origin": origin or None,
                "method": fmt or None,
                "quality": (measurement or {}).get("quality"),
            },
            "measurement_evidence": evidence,
            "raw_specifications": specs,
            "metadata_url": metadata_url,
            "provenance_note": "Independent Klippel/anechoic measurements are preferred over vendor data; links retain the originating review or developer source when published in Spinorama metadata.",
        },
        "notes": " ".join(notes),
    }


def main() -> int:
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUTPUT
    if not output.exists():
        raise FileNotFoundError(f"existing dataset not found: {output}")
    rows = json.loads(output.read_text(encoding="utf-8"))
    if not isinstance(rows, list):
        raise ValueError(f"expected existing item array, got {type(rows)}")
    speakers = get_json("/speakers")
    existing_names = {row.get("catalog_name") for row in rows if isinstance(row, dict)}
    if "Meyer Sound HD-1 (NF)" not in existing_names:
        rows.append(meyer_hd1_entry())
        existing_names.add("Meyer Sound HD-1 (NF)")
    for catalog_name in ADDITIONAL_CATALOG_MODELS:
        if catalog_name not in existing_names:
            rows.append(build_one(catalog_name, speakers))
            existing_names.add(catalog_name)
    failures: list[str] = []
    unavailable: list[str] = []
    filled: list[str] = []

    def update(row: dict) -> tuple[dict, str, str | None]:
        catalog_name = row["catalog_name"]
        clean, _ = clean_name(catalog_name)
        matched = match_speaker(clean, speakers)
        curve = row.setdefault("response_curve", {})
        if not matched:
            selected = ((row.get("research") or {}).get("selected_measurement") or {})
            if selected.get("version") == "meyer-figure-1-continuous-third-octave":
                return row, "manual", None
            # Existing measured curves still receive the required mid-band normalization.
            if curve.get("confidence") == "measured" and curve.get("points"):
                curve["points"] = normalize_midband(curve["points"])
                curve["normalized"] = True
            return row, "match_failed", None
        try:
            metadata = get_json(f"/speaker/{urllib.parse.quote(matched, safe='')}/metadata")
            version, _ = choose_measurement(metadata)
            if not version:
                return row, "unavailable", matched
            points, curve_name = fetch_curve(matched, version)
            if not points or curve_name != "Listening Window":
                return row, "unavailable", matched
            url = (f"https://api.spinorama.org/v1/speaker/{urllib.parse.quote(matched, safe='')}"
                   f"/version/{urllib.parse.quote(version, safe='')}/measurements/CEA2034")
            was_missing = curve.get("confidence") != "measured" or not curve.get("points")
            curve.update({
                "confidence": "measured",
                "source": "Spinorama Listening Window",
                "source_url": url,
                "points": points,
                "normalized": True,
            })
            sources = row.setdefault("sources", [])
            if url not in sources:
                sources.append(url)
            research = row.get("research")
            if isinstance(research, dict):
                research["spinorama_match"] = matched
            return row, "filled" if was_missing else "refreshed", matched
        except (urllib.error.URLError, ValueError, KeyError, json.JSONDecodeError) as error:
            # Do not destroy a previously measured curve because of a transient API error.
            if curve.get("confidence") == "measured" and curve.get("points"):
                curve["points"] = normalize_midband(curve["points"])
                curve["normalized"] = True
            return row, f"error: {error}", matched

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(update, rows))
    rows = [row for row, _, _ in results]
    for row, status, matched in results:
        name = row["catalog_name"]
        if status == "filled":
            filled.append(f"{name} -> {matched}")
        elif status == "manual":
            continue
        elif status == "match_failed":
            failures.append(name)
        elif status.startswith("error:"):
            unavailable.append(f"{name} -> {matched} ({status})")
        elif status == "unavailable":
            unavailable.append(f"{name} -> {matched} (default CEA2034 Listening Window unavailable)")

    output.write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    measured = sum(bool(row["response_curve"].get("confidence") == "measured" and
                        row["response_curve"].get("points")) for row in rows)
    print(f"updated {output}: {len(rows)} models, {measured} measured, {len(filled)} newly filled")
    print(f"\nNEWLY FILLED ({len(filled)}):")
    print("\n".join(filled) if filled else "(none)")
    print(f"\nMATCH FAILED ({len(failures)}):")
    print("\n".join(failures) if failures else "(none)")
    print(f"\nMATCHED BUT DEFAULT LW UNAVAILABLE ({len(unavailable)}):")
    print("\n".join(unavailable) if unavailable else "(none)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
