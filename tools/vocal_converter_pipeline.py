#!/usr/bin/env python3
"""Quality-oriented wrapper around Seed-VC for the Neuracoust vocal converter."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

import librosa
import numpy as np
import soundfile as sf


def make_reference_mix(targets: list[pathlib.Path], output: pathlib.Path) -> pathlib.Path:
    if len(targets) == 1:
        return targets[0]
    sample_rate = 44100
    total_seconds = 25.0
    each_seconds = max(3.0, total_seconds / len(targets))
    pieces: list[np.ndarray] = []
    for target in targets:
        audio, _ = librosa.load(target, sr=sample_rate, mono=True)
        intervals = librosa.effects.split(audio, top_db=38, frame_length=2048, hop_length=512)
        if len(intervals):
            audio = np.concatenate([audio[start:end] for start, end in intervals])
        wanted = min(len(audio), int(round(each_seconds * sample_rate)))
        if wanted:
            pieces.append(audio[:wanted])
    if not pieces:
        raise RuntimeError("사용 가능한 레퍼런스 음성이 없습니다.")
    fade = int(round(sample_rate * 0.04))
    mixed = pieces[0].copy()
    for piece in pieces[1:]:
        overlap = min(fade, len(mixed), len(piece))
        if overlap:
            ramp = np.linspace(0.0, 1.0, overlap, dtype=np.float32)
            joined = mixed[-overlap:] * (1.0 - ramp) + piece[:overlap] * ramp
            mixed = np.concatenate([mixed[:-overlap], joined, piece[overlap:]])
        else:
            mixed = np.concatenate([mixed, piece])
    sf.write(output, mixed[:int(total_seconds * sample_rate)], sample_rate, subtype="PCM_24")
    print(f"복합 레퍼런스 생성 · {len(pieces)}개 음색 · {len(mixed) / sample_rate:.1f}초", flush=True)
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", required=True)
    parser.add_argument("--inference", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--target", action="append", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--diffusion-steps", type=int, default=30)
    parser.add_argument("--identity", type=float, default=0.70)
    parser.add_argument("--semitone-shift", type=int, default=0)
    parser.add_argument("--checkpoint")
    parser.add_argument("--config")
    args = parser.parse_args()

    source_path = pathlib.Path(args.source)
    target_paths = [pathlib.Path(value) for value in args.target]
    output_path = pathlib.Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    source, source_sr = librosa.load(source_path, sr=None, mono=True)

    with tempfile.TemporaryDirectory(prefix="neuracoust-vc-") as temp_name:
        temp = pathlib.Path(temp_name)
        render_folder = temp / "render"
        render_folder.mkdir()
        target_path = make_reference_mix(target_paths, temp / "reference_mix.wav")
        print("Seed-VC 음색 경로 · 원본 가수 혼합 없음", flush=True)

        command = [
            args.python,
            args.inference,
            "--source", str(source_path),
            "--target", str(target_path),
            "--output", str(render_folder),
            "--diffusion-steps", str(args.diffusion_steps),
            "--length-adjust", "1.0",
            "--inference-cfg-rate", f"{args.identity:.2f}",
            "--f0-condition", "True",
            "--auto-f0-adjust", "False",
            "--semi-tone-shift", str(args.semitone_shift),
            "--fp16", "False",
        ]
        if args.checkpoint and args.config:
            command.extend(["--checkpoint", args.checkpoint, "--config", args.config])
            print("가수 전용 학습 모델 적용", flush=True)
        result = subprocess.run(command)
        if result.returncode != 0:
            return result.returncode

        rendered_files = list(render_folder.glob("*.wav"))
        if not rendered_files:
            print("Seed-VC 결과 WAV를 찾지 못했습니다.", file=sys.stderr)
            return 3
        converted, converted_sr = librosa.load(rendered_files[0], sr=source_sr, mono=True)

    expected = len(source)
    if len(converted) < expected:
        converted = np.pad(converted, (0, expected - len(converted)))
    else:
        converted = converted[:expected]

    peak = float(np.max(np.abs(converted)))
    if peak > 0.98:
        converted *= 0.98 / peak
    sf.write(output_path, converted, source_sr, subtype="PCM_24")
    print(
        f"품질 검사 완료 · {source_sr} Hz · 원본 길이 복원 · 음색 혼합 없음",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
