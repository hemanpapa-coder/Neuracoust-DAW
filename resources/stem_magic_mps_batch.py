#!/usr/bin/env python3
"""
Neuracoust DAW Stem Magic MPS batch bridge.

The DAW launches this wrapper with:
  stem_magic_mps_batch.py <input_audio> <output_dir> <model_path> <mps_server_path>

It keeps Stem Magic inside the DAW flow by driving the existing MPS inference
server protocol and writing the rendered stem WAV files only after inference.
"""
import os
import struct
import subprocess
import sys
import time

import numpy as np
import soundfile as sf


MAGIC_REQ = 0xDEADBEEF
MAGIC_RESP = 0xBEEFDEAD
def log(line: str) -> None:
    print(line, flush=True)


def read_exact(pipe, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = pipe.read(size - len(data))
        if not chunk:
            raise RuntimeError("MPS server closed the stream before sending a complete response")
        data.extend(chunk)
    return bytes(data)


def read_server_ready(pipe) -> None:
    header = read_exact(pipe, 20)
    magic, status, a, b, c = struct.unpack("<Iiiii", header)
    if magic != MAGIC_RESP or status != 0:
        raise RuntimeError(f"MPS server did not become ready: magic={hex(magic)} status={status} {a}/{b}/{c}")


def main() -> int:
    if len(sys.argv) < 5:
        log("ERROR:인수 부족: <input_audio> <output_dir> <model_path> <mps_server_path>")
        return 1

    input_path = sys.argv[1]
    output_dir = sys.argv[2]
    model_path = sys.argv[3]
    server_path = sys.argv[4]
    is_drum_split = "drumsep" in os.path.basename(server_path).lower()
    stem_names = ["Kick", "Snare", "Cymbals", "Toms"] if is_drum_split else ["Drums", "Bass", "Other", "Vocals"]

    if not os.path.isfile(input_path):
        log(f"ERROR:입력 오디오를 찾을 수 없습니다: {input_path}")
        return 1
    if not os.path.isfile(server_path):
        log(f"ERROR:Stem Magic MPS 서버를 찾을 수 없습니다: {server_path}")
        return 1

    os.makedirs(output_dir, exist_ok=True)
    log(f"DEVICE:Metal/MPS server bridge ({os.path.basename(server_path)})")
    log(f"INFO:분리 모드: {'드럼 파트' if is_drum_split else '4 Stem'}")
    if model_path and os.path.exists(model_path):
        log(f"INFO:로컬 모델 후보 확인: {os.path.basename(model_path)}")
    log("PROGRESS:0.0100")

    try:
        data, sr = sf.read(input_path, dtype="float32", always_2d=True)
    except Exception as exc:
        log(f"ERROR:오디오 로드 실패: {exc}")
        return 1
    log("PROGRESS:0.0300")

    if data.shape[1] == 1:
        data = np.repeat(data, 2, axis=1)
    elif data.shape[1] > 2:
        data = data[:, :2]

    audio = np.ascontiguousarray(data.T, dtype=np.float32)
    n_ch, n_samp = audio.shape
    log(f"INFO:오디오 로드 완료 (sr={sr}, channels={n_ch}, samples={n_samp})")
    log("PROGRESS:0.0500")

    log("INFO:Stem Magic Metal/MPS 서버 시작")
    server_args = [sys.executable, server_path]
    if not is_drum_split:
        server_args += ["--model", "htdemucs_ft", "--shifts", "1", "--overlap", "0.25"]
    proc = subprocess.Popen(
        server_args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        read_server_ready(proc.stdout)
        log("INFO:Stem Magic Metal/MPS 서버 준비 완료")
        log("PROGRESS:0.1000")

        assert proc.stdin is not None
        log("INFO:오디오를 Metal/MPS 서버로 전송")
        proc.stdin.write(struct.pack("<Iiii", MAGIC_REQ, n_ch, n_samp, 0))
        proc.stdin.write(audio.tobytes())
        proc.stdin.flush()
        log("PROGRESS:0.2500")

        response_header = read_exact(proc.stdout, 16)
        magic, stem_count, response_channels, response_samples = struct.unpack("<Iiii", response_header)
        if magic != MAGIC_RESP or stem_count != len(stem_names) or response_channels <= 0 or response_samples <= 0:
            raise RuntimeError(
                f"잘못된 MPS 응답: magic={hex(magic)} stems={stem_count} channels={response_channels} samples={response_samples}"
            )
        log("INFO:Stem Magic 응답 수신 중")
        log("PROGRESS:0.7000")

        frame_values = stem_count * response_channels * response_samples
        payload = read_exact(proc.stdout, frame_values * 4)
        log("PROGRESS:0.8000")
        stems = np.frombuffer(payload, dtype=np.float32).reshape(stem_count, response_channels, response_samples)
        stems = np.nan_to_num(stems, nan=0.0, posinf=1.0, neginf=-1.0)
        stems = np.clip(stems, -1.0, 1.0)
        log("PROGRESS:0.8500")

        base_name = os.path.splitext(os.path.basename(input_path))[0]
        for index, stem_name in enumerate(stem_names):
            out_path = os.path.join(output_dir, f"{base_name}_{stem_name}.wav")
            sf.write(out_path, stems[index].T, sr, subtype="PCM_16")
            log(f"INFO:{stem_name}.wav 저장 완료")
            log(f"PROGRESS:{0.88 + ((index + 1) / len(stem_names)) * 0.10:.4f}")

        log("PROGRESS:1.0000")
        log("DONE")
        return 0
    except Exception as exc:
        log(f"ERROR:{exc}")
        return 1
    finally:
        try:
            if proc.stdin is not None:
                proc.stdin.close()
        except Exception:
            pass
        deadline = time.time() + 2.0
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.05)
        if proc.poll() is None:
            proc.terminate()
        stderr = b""
        try:
            stderr = proc.stderr.read() if proc.stderr is not None else b""
        except Exception:
            stderr = b""
        if stderr:
            for line in stderr.decode("utf-8", errors="replace").splitlines()[-8:]:
                log(f"INFO:MPS {line}")


if __name__ == "__main__":
    sys.exit(main())
