#!/usr/bin/env python3
"""
Neuracoust DAW Stem Magic CoreML/ANE batch bridge.

Usage:
  stem_magic_coreml_batch.py <input_audio> <output_dir> <coreml_model_path>

The model is expected to accept stereo float32 audio shaped like
[1, 2, samples] and return four stereo stems shaped like [1, 4, 2, samples]
or [4, 2, samples]. It runs with Core ML CPU_AND_NE compute units so Apple
Silicon can schedule supported layers on the Neural Engine.
"""
import os
import sys

import numpy as np
import soundfile as sf


def log(line: str) -> None:
    print(line, flush=True)


def main() -> int:
    if len(sys.argv) < 4:
        log("ERROR:인수 부족: <input_audio> <output_dir> <coreml_model_path>")
        return 1

    input_path = sys.argv[1]
    output_dir = sys.argv[2]
    model_path = sys.argv[3]
    stem_names = ["Drums", "Bass", "Other", "Vocals"]

    if not os.path.isfile(input_path):
        log(f"ERROR:입력 오디오를 찾을 수 없습니다: {input_path}")
        return 1
    if not os.path.exists(model_path):
        log(f"ERROR:CoreML/ANE stem 모델을 찾을 수 없습니다: {model_path}")
        return 1

    try:
        import coremltools as ct
    except Exception as exc:
        log(f"ERROR:coremltools를 불러올 수 없습니다: {exc}")
        return 1

    os.makedirs(output_dir, exist_ok=True)
    log(f"DEVICE:CoreML/ANE bridge ({os.path.basename(model_path)})")
    log("INFO:분리 모드: 4 Stem")
    log("PROGRESS:0.0100")

    try:
        model = ct.models.MLModel(model_path, compute_units=ct.ComputeUnit.CPU_AND_NE)
    except Exception as exc:
        log(f"ERROR:CoreML 모델 로드 실패: {exc}")
        return 1
    log("INFO:CoreML 모델 로드 완료 · CPU_AND_NE")
    log("PROGRESS:0.0800")

    try:
        spec = model.get_spec()
        input_desc = spec.description.input[0]
        output_desc = spec.description.output[0]
        input_name = input_desc.name
        output_name = output_desc.name
        shape = list(input_desc.type.multiArrayType.shape)
        chunk_samples = int(shape[-1]) if shape else 0
    except Exception as exc:
        log(f"ERROR:CoreML 모델 입출력 정보 읽기 실패: {exc}")
        return 1

    if chunk_samples <= 0:
        log("ERROR:CoreML 모델 입력 sample 길이를 확인할 수 없습니다")
        return 1

    try:
        data, sr = sf.read(input_path, dtype="float32", always_2d=True)
    except Exception as exc:
        log(f"ERROR:오디오 로드 실패: {exc}")
        return 1
    log("PROGRESS:0.1200")

    if data.shape[1] == 1:
        data = np.repeat(data, 2, axis=1)
    elif data.shape[1] > 2:
        data = data[:, :2]

    audio = np.ascontiguousarray(data.T, dtype=np.float32)
    total_samples = audio.shape[1]
    log(f"INFO:오디오 로드 완료 (sr={sr}, channels=2, samples={total_samples})")
    log(f"INFO:CoreML chunk samples={chunk_samples}")
    log("PROGRESS:0.1600")

    stem_chunks = [[] for _ in stem_names]
    processed = 0
    while processed < total_samples:
        end = min(total_samples, processed + chunk_samples)
        chunk = audio[:, processed:end]
        valid = chunk.shape[1]
        if valid < chunk_samples:
            padded = np.zeros((2, chunk_samples), dtype=np.float32)
            padded[:, :valid] = chunk
            chunk = padded
        model_input = chunk.reshape(1, 2, chunk_samples).astype(np.float32, copy=False)
        try:
            prediction = model.predict({input_name: model_input})
            output = prediction.get(output_name)
            if output is None:
                output = next(iter(prediction.values()))
        except Exception as exc:
            log(f"ERROR:CoreML 예측 실패: {exc}")
            return 1

        stems = np.asarray(output, dtype=np.float32)
        stems = np.squeeze(stems)
        if stems.ndim != 3 or stems.shape[0] != 4:
            log(f"ERROR:예상하지 못한 CoreML 출력 shape: {stems.shape}")
            return 1
        if stems.shape[1] != 2 and stems.shape[2] == 2:
            stems = np.transpose(stems, (0, 2, 1))
        if stems.shape[1] != 2:
            log(f"ERROR:CoreML 출력 channel shape 오류: {stems.shape}")
            return 1
        stems = stems[:, :, :valid]
        stems = np.nan_to_num(stems, nan=0.0, posinf=1.0, neginf=-1.0)
        stems = np.clip(stems, -1.0, 1.0)
        for index in range(4):
            stem_chunks[index].append(stems[index])

        processed = end
        progress = 0.16 + 0.68 * (processed / max(1, total_samples))
        log(f"PROGRESS:{progress:.4f}")

    base_name = os.path.splitext(os.path.basename(input_path))[0]
    for index, stem_name in enumerate(stem_names):
        stem = np.concatenate(stem_chunks[index], axis=1) if stem_chunks[index] else np.zeros((2, 0), dtype=np.float32)
        out_path = os.path.join(output_dir, f"{base_name}_{stem_name}.wav")
        sf.write(out_path, stem.T, sr, subtype="PCM_16")
        log(f"INFO:{stem_name}.wav 저장 완료")
        log(f"PROGRESS:{0.88 + ((index + 1) / len(stem_names)) * 0.10:.4f}")

    log("PROGRESS:1.0000")
    log("DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
