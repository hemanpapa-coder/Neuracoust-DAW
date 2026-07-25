#!/usr/bin/env python3
"""Export Demucs **htdemucs_6s** (6 sources: drums/bass/other/vocals/guitar/piano) to a TorchScript .pt
that DW's neuracoust_stem_separator can load — the SAME fixed-chunk format as the existing demucs.pt,
so nothing in the C++ helper changes (it reads the source count from the model's output automatically).

WHAT YOU NEED TO DO
-------------------
1. Install Demucs + PyTorch (2.x, ideally 2.5.x so it loads in DW's LibTorch 2.5.1):
       python3 -m pip install -U demucs torch
2. Run this script:
       python3 tools/export_demucs_6s_torchscript.py
   It writes htdemucs_6s.pt next to demucs.pt in the "Neuracoust Stem Magic" folder.
3. Rebuild DW — CMake bundles htdemucs_6s.pt and the clip menu's "6파트 (+기타/피아노)" option appears.

The helper feeds fixed 343980-sample chunks (htdemucs' training_segment @ 44.1 kHz), so the model is
traced at exactly that length. If tracing fails on your Demucs version, tell me the error.
"""
import os
import sys

CHUNK = 343980   # htdemucs training_segment @ 44.1 kHz — the exact length the C++ helper feeds.


def main() -> int:
    default_out = "/Volumes/Program Dev/Neuracoust Stem Magic"
    out_dir = sys.argv[1] if len(sys.argv) > 1 else default_out
    try:
        import torch
        from demucs.pretrained import get_model
    except ImportError:
        print("Demucs / PyTorch not installed. Run:\n    python3 -m pip install -U demucs torch", file=sys.stderr)
        return 2

    print("Loading htdemucs_6s (downloads weights on first run) …")
    bag = get_model("htdemucs_6s")
    bag.eval()
    # A pretrained name resolves to a BagOfModels; htdemucs_6s wraps a single HTDemucs. Trace that model
    # at the fixed chunk length — the same raw [1,2,N] → [1,S,2,N] contract as the existing demucs.pt.
    model = bag.models[0] if getattr(bag, "models", None) else bag
    model.eval()

    example = torch.zeros(1, 2, CHUNK, dtype=torch.float32)
    with torch.no_grad():
        try:
            traced = torch.jit.trace(model, example)
        except Exception as e:  # noqa: BLE001
            print(f"trace failed: {e}\nTry: pip install -U 'demucs' 'torch' — or send me the error.", file=sys.stderr)
            return 1
        out = traced(torch.zeros(1, 2, CHUNK))

    if out.dim() != 4 or out.shape[1] != 6:
        print(f"unexpected output shape {tuple(out.shape)} (expected [1, 6, 2, {CHUNK}])", file=sys.stderr)
        return 1

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "htdemucs_6s.pt")
    traced.save(path)
    size_mb = os.path.getsize(path) / (1024 * 1024)
    print(f"  ✓ {path}  ({size_mb:.0f} MB, output {tuple(out.shape)} = 6 sources)")
    print("\nDone. Rebuild DW; the '6파트 (+기타/피아노)' menu option will light up.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
