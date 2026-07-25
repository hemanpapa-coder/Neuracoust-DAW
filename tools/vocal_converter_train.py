#!/usr/bin/env python3
"""Prepare singer data and fine-tune Seed-VC on Apple Silicon."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

import librosa
import numpy as np
import soundfile as sf


SUPPORTED = {".wav", ".flac", ".mp3", ".m4a", ".opus", ".ogg", ".aif", ".aiff"}


def audio_files(folders: list[pathlib.Path]) -> list[pathlib.Path]:
    result: list[pathlib.Path] = []
    for folder in folders:
        if not folder.exists():
            continue
        result.extend(
            path for path in folder.rglob("*")
            if path.is_file() and path.suffix.lower() in SUPPORTED
        )
    return sorted(set(result))


def fade_edges(audio: np.ndarray, sample_rate: int) -> np.ndarray:
    fade = min(len(audio) // 2, int(round(sample_rate * 0.025)))
    if fade > 1:
        ramp = np.linspace(0.0, 1.0, fade, dtype=np.float32)
        audio[:fade] *= ramp
        audio[-fade:] *= ramp[::-1]
    return audio


def prepare_dataset(inputs: list[pathlib.Path], output: pathlib.Path) -> tuple[int, float]:
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    sample_rate = 44100
    written = 0
    total_seconds = 0.0

    for source in inputs:
        try:
            audio, _ = librosa.load(source, sr=sample_rate, mono=True)
        except Exception as error:
            print(f"건너뜀 · {source.name} · {error}", flush=True)
            continue
        if len(audio) < sample_rate:
            continue

        intervals = librosa.effects.split(audio, top_db=38, frame_length=2048, hop_length=512)
        for interval_start, interval_end in intervals:
            phrase = audio[interval_start:interval_end]
            cursor = 0
            maximum = sample_rate * 14
            minimum = sample_rate * 2
            while cursor < len(phrase):
                chunk = phrase[cursor:min(len(phrase), cursor + maximum)].copy()
                cursor += maximum
                if len(chunk) < minimum:
                    continue
                peak = float(np.max(np.abs(chunk)))
                rms = float(np.sqrt(np.mean(chunk * chunk) + 1.0e-12))
                if peak < 0.01 or rms < 0.002:
                    continue
                chunk = fade_edges(chunk, sample_rate)
                if peak > 0.95:
                    chunk *= 0.95 / peak
                destination = output / f"clip_{written:05d}.wav"
                sf.write(destination, chunk, sample_rate, subtype="PCM_24")
                written += 1
                total_seconds += len(chunk) / sample_rate

    return written, total_seconds


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--input-folder", action="append", default=[])
    parser.add_argument("--input-file", action="append", default=[])
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--model-output", required=True)
    parser.add_argument("--run-name", required=True)
    parser.add_argument("--steps", type=int, default=300)
    args = parser.parse_args()

    repo = pathlib.Path(args.repo)
    dataset = pathlib.Path(args.dataset)
    model_output = pathlib.Path(args.model_output)
    sources = audio_files([pathlib.Path(value) for value in args.input_folder])
    sources.extend(
        path for value in args.input_file
        if (path := pathlib.Path(value)).exists() and path.suffix.lower() in SUPPORTED
    )
    sources = sorted(set(sources))
    if not sources:
        print("학습할 보컬 파일이 없습니다.", file=sys.stderr)
        return 2

    clips, seconds = prepare_dataset(sources, dataset)
    print(f"학습 데이터 준비 완료 · {clips}구간 · {seconds / 60.0:.1f}분", flush=True)
    if clips < 3 or seconds < 20:
        print("학습에는 최소 3구간, 총 20초 이상의 깨끗한 보컬이 필요합니다.", file=sys.stderr)
        return 3

    config = repo / "configs/presets/config_dit_mel_seed_uvit_whisper_base_f0_44k.yml"
    command = [
        sys.executable,
        str(repo / "train.py"),
        "--config", str(config),
        "--dataset-dir", str(dataset),
        "--run-name", args.run_name,
        "--batch-size", "1",
        "--max-steps", str(args.steps),
        "--max-epochs", "1000",
        "--save-every", str(max(50, min(args.steps, 250))),
        "--num-workers", "0",
    ]
    print(f"가수 전용 모델 학습 시작 · {args.steps} steps · Apple MPS", flush=True)
    result = subprocess.run(command, cwd=repo)
    if result.returncode != 0:
        return result.returncode

    run_folder = repo / "runs" / args.run_name
    checkpoint = run_folder / "ft_model.pth"
    if not checkpoint.exists():
        print("완성된 체크포인트를 찾지 못했습니다.", file=sys.stderr)
        return 4

    model_output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(checkpoint, model_output / "ft_model.pth")
    shutil.copy2(config, model_output / "config.yml")
    print(f"전용 모델 완성 · {model_output / 'ft_model.pth'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
