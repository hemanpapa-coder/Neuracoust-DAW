#!/usr/bin/env python3
"""Polyphonic audio → MIDI transcription for DW, using Spotify's **basic-pitch** (a lightweight, instrument-
agnostic note-transcription model). Spawned as a subprocess by the DAW — the same line protocol as the
stem separator — so its (TensorFlow) dependency stays out of the audio process.

    neuracoust_basic_pitch <input.wav>

stdout, one line per detected note plus a terminator:
    NOTE <midiPitch> <startSeconds> <durationSeconds> <velocity 1-127>
    DONE <count>      |      ERROR <message>

The DAW turns each NOTE into a MIDI note on a new instrument track. Unlike the built-in CREPE/YIN path
(monophonic), basic-pitch resolves CHORDS — piano, guitar, layered parts.

SETUP (once): the user installs basic-pitch into the python3 the app calls:
    python3 -m pip install basic-pitch
If it isn't installed, this prints an ERROR line the DAW surfaces (it does not crash the app).
"""
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("ERROR usage: neuracoust_basic_pitch <input.wav>")
        return 2
    audio_path = sys.argv[1]

    try:
        # Importing basic_pitch pulls in TensorFlow; keep it inside main so a missing dep is a clean ERROR.
        from basic_pitch.inference import predict
        from basic_pitch import ICASSP_2022_MODEL_PATH
    except Exception as e:  # noqa: BLE001
        print(f"ERROR basic_pitch not available ({e.__class__.__name__}); run: python3 -m pip install basic-pitch")
        return 1

    try:
        # predict() returns (model_output, midi_data, note_events). note_events is a list of
        # (start_s, end_s, pitch_midi, amplitude 0..1, [pitch_bends]).
        _, _, note_events = predict(audio_path, ICASSP_2022_MODEL_PATH)
    except Exception as e:  # noqa: BLE001
        print(f"ERROR transcription failed: {e}")
        return 1

    count = 0
    for ev in note_events:
        start_s, end_s, pitch = ev[0], ev[1], int(ev[2])
        amp = ev[3] if len(ev) > 3 else 0.7
        dur = max(0.02, end_s - start_s)
        if pitch < 0 or pitch > 127:
            continue
        vel = int(max(1, min(127, round(amp * 127))))
        print(f"NOTE {pitch} {start_s:.4f} {dur:.4f} {vel}")
        count += 1
    print(f"DONE {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
