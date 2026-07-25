#!/usr/bin/env python3
"""Selected four-stem separation on Apple Metal (MPS).

The DAW always supplies a WAV containing exactly the selected clip window.
stdout is a small line protocol consumed by EngineController.
"""

import argparse
import contextlib
import importlib.util
import os
import sys
from pathlib import Path

import soundfile as sf
import torch


def emit(line: str) -> None:
    print(line, flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output_dir")
    parser.add_argument("--stem-prefix", default="clip")
    parser.add_argument("--stems", default="drums,bass,other,vocals")
    parser.add_argument("--drum-detail", default="")
    parser.add_argument("--other-detail", default="")
    args = parser.parse_args()

    if not torch.backends.mps.is_available() or not torch.backends.mps.is_built():
        emit("ERROR Metal MPS를 사용할 수 없습니다")
        return 2

    wanted = {value.strip().lower() for value in args.stems.split(",") if value.strip()}
    wanted_drums = {value.strip().lower() for value in args.drum_detail.split(",") if value.strip()}
    wanted_other = {value.strip().lower() for value in args.other_detail.split(",") if value.strip()}
    device = torch.device("mps")
    try:
        from demucs.apply import apply_model
        from demucs.pretrained import get_model
        with contextlib.redirect_stdout(sys.stderr):
            model = get_model("htdemucs_ft")
        model.eval().to(device)
        audio, sample_rate = sf.read(args.input, dtype="float32", always_2d=True)
        tensor = torch.from_numpy(audio.T).unsqueeze(0)
        if tensor.shape[1] == 1:
            tensor = tensor.repeat(1, 2, 1)
        elif tensor.shape[1] > 2:
            tensor = tensor[:, :2, :]
        emit("PROGRESS 0.08")
        with torch.inference_mode():
            result = apply_model(model, tensor.to(device), shifts=1, split=True,
                                 overlap=0.25, progress=False)[0].cpu()
        emit("PROGRESS 0.88")
    except Exception as exc:
        emit(f"ERROR Metal 분리 실패: {exc}")
        return 1

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    source_indices = {name: model.sources.index(name) for name in ("drums", "bass", "other", "vocals")}
    labels = {"drums": "Drums", "bass": "Bass", "other": "Other", "vocals": "Vocals"}

    # A two-part request is still computed from the same complementary four-stem basis, but it is
    # published explicitly as Vocals + Accompaniment instead of pretending to be "automatic".
    if wanted == {"vocals", "accompaniment"}:
        vocals = result[source_indices["vocals"]]
        accompaniment = sum(result[source_indices[name]] for name in ("drums", "bass", "other"))
        for label, stem in (("Vocals", vocals), ("Accompaniment", accompaniment)):
            path = output_dir / f"{args.stem_prefix}_{label}.wav"
            sf.write(path, stem.T.numpy(), sample_rate, subtype="FLOAT")
            emit(f"STEM {label} {path}")
    else:
        for name in ("drums", "bass", "other", "vocals"):
            if name not in wanted:
                continue
            if name == "drums" and wanted_drums:
                continue
            if name == "other" and wanted_other:
                continue
            label = labels[name]
            path = output_dir / f"{args.stem_prefix}_{label}.wav"
            sf.write(path, result[source_indices[name]].T.numpy(), sample_rate, subtype="FLOAT")
            emit(f"STEM {label} {path}")

        if "drums" in wanted and wanted_drums:
            try:
                server_path = Path("/Volumes/Program Dev/Neuracoust Stem Magic (Metal)/scripts/drumsep_inference_server.py")
                spec = importlib.util.spec_from_file_location("neuracoust_drumsep", server_path)
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                module.download_model()
                drum_model = module.load_drumsep(device)
                drum_input = result[source_indices["drums"]].unsqueeze(0).to(device)
                with torch.inference_mode():
                    drum_result = apply_model(drum_model, drum_input, shifts=0, split=True,
                                              overlap=0.25, progress=False)[0].cpu()
                for index, name in enumerate(("kick", "snare", "cymbals", "toms")):
                    if name not in wanted_drums:
                        continue
                    label = {"kick": "Kick", "snare": "Snare", "cymbals": "Cymbals", "toms": "Toms"}[name]
                    path = output_dir / f"{args.stem_prefix}_{label}.wav"
                    sf.write(path, drum_result[index].T.numpy(), sample_rate, subtype="FLOAT")
                    emit(f"STEM {label} {path}")
            except Exception as exc:
                emit(f"ERROR 드럼 세부 분리 실패: {exc}")
                return 1

        if "other" in wanted and wanted_other:
            try:
                with contextlib.redirect_stdout(sys.stderr):
                    other_model = get_model("htdemucs_6s")
                other_model.eval().to(device)
                other_input = result[source_indices["other"]].unsqueeze(0).to(device)
                with torch.inference_mode():
                    other_result = apply_model(other_model, other_input, shifts=1, split=True,
                                               overlap=0.25, progress=False)[0].cpu()
                detail_sum = torch.zeros_like(result[source_indices["other"]])
                for name in ("guitar", "piano"):
                    if name not in wanted_other:
                        continue
                    stem = other_result[other_model.sources.index(name)]
                    detail_sum += stem
                    label = name.title()
                    path = output_dir / f"{args.stem_prefix}_{label}.wav"
                    sf.write(path, stem.T.numpy(), sample_rate, subtype="FLOAT")
                    emit(f"STEM {label} {path}")
                remainder = result[source_indices["other"]] - detail_sum
                path = output_dir / f"{args.stem_prefix}_Other Remainder.wav"
                sf.write(path, remainder.T.numpy(), sample_rate, subtype="FLOAT")
                emit(f"STEM Other-Remainder {path}")
            except Exception as exc:
                emit(f"ERROR 악기 세부 분리 실패: {exc}")
                return 1

    emit("PROGRESS 1.0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
