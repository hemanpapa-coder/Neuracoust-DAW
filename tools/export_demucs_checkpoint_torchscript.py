#!/usr/bin/env python3
"""Trace ANY Demucs-architecture checkpoint (.th / .pt bag) to a fixed-chunk TorchScript .pt that DW's
neuracoust_stem_separator can load — the SAME [1,2,N] → [1,S,2,N] contract as demucs.pt / htdemucs_6s.pt,
so the C++ helper needs no changes (it reads the source count from the model output). This is how the
DRUM-SPLIT and the experimental ORCHESTRA-FAMILY models get exported.

    python3 tools/export_demucs_checkpoint_torchscript.py <checkpoint> <output.pt>

DRUM SPLIT  (kick / snare / toms / cymbals — 5-source variants add a separate hihat)
------------------------------------------------------------------------------------
Use a Demucs-v4 "drumsep" checkpoint (a retrain of htdemucs on isolated drum stems). These are NOT in
Demucs' pretrained registry, so you point at the downloaded .th file:

    python3 tools/export_demucs_checkpoint_torchscript.py ~/Downloads/drumsep.th \
        "/Volumes/Program Dev/Neuracoust Stem Magic/drumsep.pt"

The C++ helper names the outputs by count when run with --kind drum:
    4 sources → Kick / Snare / Toms / Cymbals
    5 sources → Kick / Snare / Toms / HiHat / Cymbals
Then DW's "스템 분리 ▸ 드럼 분해" runs it, and "텀 음고별 분리" (--split-toms) splits the Toms stem.

ORCHESTRA FAMILY  (strings / brass / woodwinds / … — EXPERIMENTAL, honest caveat)
---------------------------------------------------------------------------------
There is NO production-grade model that cleanly separates oboe from clarinet, or trumpet from trombone,
in a dense mix — general music separation folds all of them into "other". The best that exists is a
COARSE family split (strings / brass / woodwinds / other), and only from a checkpoint trained for it
(e.g. on URMP / EnsembleSet-derived data). If you have such a Demucs-arch checkpoint:

    python3 tools/export_demucs_checkpoint_torchscript.py <orchestra_family.th> \
        "/Volumes/Program Dev/Neuracoust Stem Magic/orchestra.pt"

DW names it with --kind orchestra (4 → Strings/Brass/Woodwinds/Other). Individual solo-instrument
separation is out of scope until a model that actually does it exists; this is the plumbing, ready.

Rebuild DW after exporting — CMake bundles drumsep.pt / orchestra.pt and lights up the matching menu
option (like the 6-part one).
"""
import os
import sys

CHUNK = 343980   # htdemucs training_segment @ 44.1 kHz — the exact length the C++ helper feeds.


def load_any_demucs(path):
    """Load a Demucs checkpoint from a pretrained name, a bag .yaml, or a raw .th state file."""
    from demucs.pretrained import get_model
    # A bare name (or a repo bag) resolves through get_model; a file path may be a .th state.
    if os.path.isfile(path) and path.endswith((".th", ".pt", ".ckpt")):
        try:
            from demucs.states import load_model
            return load_model(path)
        except Exception:  # noqa: BLE001 — fall through to get_model, some bags load by path too
            pass
    return get_model(path)


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: export_demucs_checkpoint_torchscript.py <checkpoint> <output.pt>", file=sys.stderr)
        return 2
    ckpt, out_path = sys.argv[1], sys.argv[2]
    try:
        import torch
    except ImportError:
        print("PyTorch not installed. Run:\n    python3 -m pip install -U demucs torch", file=sys.stderr)
        return 2

    print(f"Loading checkpoint: {ckpt}")
    try:
        bag = load_any_demucs(ckpt)
    except Exception as e:  # noqa: BLE001
        print(f"load failed: {e}\nInstall demucs (pip install -U demucs) and check the path.", file=sys.stderr)
        return 1
    bag.eval()
    # A pretrained name / bag resolves to a BagOfModels wrapping one or more models; trace the first.
    model = bag.models[0] if getattr(bag, "models", None) else bag
    model.eval()

    example = torch.zeros(1, 2, CHUNK, dtype=torch.float32)
    with torch.no_grad():
        try:
            traced = torch.jit.trace(model, example)
        except Exception as e:  # noqa: BLE001
            print(f"trace failed: {e}\nThis needs a Demucs-architecture checkpoint (HTDemucs).", file=sys.stderr)
            return 1
        out = traced(torch.zeros(1, 2, CHUNK))

    if out.dim() != 4 or out.shape[3] != CHUNK:
        print(f"unexpected output shape {tuple(out.shape)} (expected [1, S, 2, {CHUNK}])", file=sys.stderr)
        return 1
    n_sources = int(out.shape[1])

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    traced.save(out_path)
    size_mb = os.path.getsize(out_path) / (1024 * 1024)
    print(f"  ✓ {out_path}  ({size_mb:.0f} MB, output {tuple(out.shape)} = {n_sources} sources)")

    base = os.path.basename(out_path).lower()
    if "drum" in base:
        names = ("Kick/Snare/Toms/Cymbals" if n_sources == 4 else
                 "Kick/Snare/Toms/HiHat/Cymbals" if n_sources == 5 else f"{n_sources} generic stems")
        print(f"  drum model → DW will name these {names} under --kind drum.")
    elif "orch" in base:
        print(f"  orchestra model → DW names these by count under --kind orchestra ({n_sources} sources).")
    print("\nDone. Rebuild DW; the matching menu option will light up.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
