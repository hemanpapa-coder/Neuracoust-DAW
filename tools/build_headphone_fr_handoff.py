#!/usr/bin/env python3
"""Build a source-preserving handoff package for the DAW headphone catalog."""

from __future__ import annotations

import csv
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from urllib.parse import quote


CATALOG = [
    "Sennheiser HD 600", "Sennheiser HD 650", "Sennheiser HD 800S",
    "Sennheiser HD 25", "Sennheiser HD 280 Pro", "Sennheiser HD 560S",
    "Sennheiser HD 660 S", "Sennheiser HD 660S2", "Sennheiser HD 620S",
    "Sennheiser HD 800", "Sennheiser HD 820",
    "Beyerdynamic DT 770 Pro", "Beyerdynamic DT 880 Pro", "Beyerdynamic DT 990 Pro",
    "Beyerdynamic DT 1990 Pro", "Beyerdynamic DT 700 Pro X", "Beyerdynamic DT 900 Pro X",
    "AKG K240 Studio", "AKG K271 MkII", "AKG K361", "AKG K371", "AKG K701",
    "AKG K702", "AKG K712 Pro", "Audio-Technica ATH-M20x",
    "Audio-Technica ATH-M40x", "Audio-Technica ATH-M50x", "Audio-Technica ATH-M60x",
    "Audio-Technica ATH-M70x", "Audio-Technica ATH-R70x", "Sony MDR-7506",
    "Sony MDR-CD900ST", "Sony MDR-MV1", "Focal Listen Pro", "Focal Clear",
    "Focal Clear Mg", "Focal Utopia", "Audeze LCD-2 Classic", "Audeze LCD-X",
    "Audeze LCD-XC", "Audeze MM-100", "Audeze MM-500", "HIFIMAN Sundara",
    "HIFIMAN Ananda", "HIFIMAN Edition XS", "HIFIMAN HE400se", "Shure SRH440",
    "Shure SRH840A", "Shure SRH1540", "Shure SRH1840", "Austrian Audio Hi-X60",
    "Dan Clark Audio E3", "Dan Clark Audio Stealth", "Fostex TH900mk2", "Grado SR325x", "Neumann NDH 20",
    "Neumann NDH 30", "Apple AirPods Max", "Sony WH-1000XM5",
]

# Only aliases which identify the same product are accepted. Modified pads and successor
# generations are intentionally not substituted.
ALIASES = {
    "Sennheiser HD 800S": ["Sennheiser HD 800 S"],
    "Beyerdynamic DT 880 Pro": ["Beyerdynamic DT 880 (250 Ohm)"],
    "Beyerdynamic DT 1990 Pro": ["Beyerdynamic DT 1990 (analytic earpads)", "Beyerdynamic DT 1990 (balanced earpads)"],
    "AKG K271 MkII": ["AKG K271 MKII"],
    "AKG K712 Pro": ["AKG K712 PRO", "AKG K712"],
    "Focal Listen Pro": ["Focal Listen Professional"],
    "Neumann NDH 20": ["Neumann NDH20"],
    "Neumann NDH 30": ["Neumann NDH30"],
}

AMBIGUOUS = {}


def safe_name(text: str) -> str:
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in text).strip("_")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: build_headphone_fr_handoff.py AUTOEQ_DIR OUTPUT_DIR", file=sys.stderr)
        return 2
    repo, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    measurements = repo / "measurements"
    if not measurements.is_dir():
        raise SystemExit(f"missing measurements directory: {measurements}")
    if out.exists():
        shutil.rmtree(out)
    curves = out / "curves"
    curves.mkdir(parents=True)
    commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    all_csv = list(measurements.rglob("*.csv"))
    rows, measurements_jsonl = [], []

    for index, model in enumerate(CATALOG):
        accepted = [model] + ALIASES.get(model, [])
        hits = sorted([p for p in all_csv if p.stem in accepted], key=lambda p: str(p).lower())
        candidate_names = []
        status = "available" if hits else "unavailable"
        if model in AMBIGUOUS:
            status = "available_with_caveat" if hits else "ambiguous_or_not_applicable"
        source_names = []
        for n, src in enumerate(hits, 1):
            rel = src.relative_to(measurements)
            source = rel.parts[0]
            source_names.append(source)
            dest_dir = curves / f"{index:02d}_{safe_name(model)}"
            dest_dir.mkdir(exist_ok=True)
            dest = dest_dir / f"{n:02d}_{safe_name(source)}__{safe_name(src.stem)}.csv"
            shutil.copy2(src, dest)
            digest = hashlib.sha256(dest.read_bytes()).hexdigest()
            with dest.open(newline="", encoding="utf-8-sig") as f:
                reader = csv.DictReader(f)
                points = list(reader)
            measurements_jsonl.append({
                "catalog_index": index, "catalog_model": model, "matched_measurement_name": src.stem,
                "match_kind": "exact" if src.stem == model else "reviewed_alias",
                "measurement_source": source, "relative_csv": str(dest.relative_to(out)),
                "columns": reader.fieldnames, "point_count": len(points), "sha256": digest,
                "autoeq_commit": commit,
                "github_file_url": "https://github.com/jaakkopasanen/AutoEq/blob/" + commit + "/measurements/" + quote(str(rel)),
            })
        rows.append({
            "catalog_index": index, "catalog_model": model, "status": status,
            "curve_file_count": len(hits), "measurement_sources": "; ".join(sorted(set(source_names))),
            "accepted_measurement_names": "; ".join(accepted),
            "caveat_or_reason": AMBIGUOUS.get(model, ""),
            "unresolved_candidates": "; ".join(candidate_names),
        })

    with (out / "catalog_status.csv").open("w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader(); w.writerows(rows)
    with (out / "measurements.jsonl").open("w", encoding="utf-8") as f:
        for item in measurements_jsonl:
            f.write(json.dumps(item, ensure_ascii=False) + "\n")
    summary = {
        "catalog_count": len(rows), "available": sum(r["status"] == "available" for r in rows),
        "available_with_caveat": sum(r["status"] == "available_with_caveat" for r in rows),
        "ambiguous_or_not_applicable": sum(r["status"] == "ambiguous_or_not_applicable" for r in rows),
        "unavailable": sum(r["status"] == "unavailable" for r in rows),
        "curve_file_count": len(measurements_jsonl), "autoeq_commit": commit,
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "README.md").write_text(f"""# Neuracoust DAW headphone frequency-response handoff

Generated from the verified-measurement catalog in `src/bridge/NeuracoustEngineBridge.cpp` and AutoEq commit `{commit}`.

## Contents

- `catalog_status.csv`: one row for every retained DAW catalog entry.
- `measurements.jsonl`: provenance, match type, point count, checksum, and source link for every copied curve.
- `curves/`: source-preserving numerical CSV files (`frequency,raw`; dB values are relative/normalized, not absolute SPL).
- `summary.json`: machine-readable counts.

## Rules used

No curve was synthesized, averaged across rigs, or substituted from a successor model. Multiple source files are retained independently because fixtures/targets are not directly interchangeable, especially above 8–10 kHz. Modified-pad measurements are excluded unless the catalog itself specifies those pads. Models previously classified as unavailable, ambiguous, generic, or conditionally measured were removed from both the DAW picker and this handoff package.

Primary numerical collection: https://github.com/jaakkopasanen/AutoEq (MIT). AutoEq aggregates measurements from the source named in each path; consult each source's terms before redistributing commercially. This package is research input, not yet a correction filter set.
""", encoding="utf-8")
    zip_path = out.with_suffix(".zip")
    if zip_path.exists(): zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for p in sorted(out.rglob("*")):
            if p.is_file(): z.write(p, Path(out.name) / p.relative_to(out))
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(zip_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
