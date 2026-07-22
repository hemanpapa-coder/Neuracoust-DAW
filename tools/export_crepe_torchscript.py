#!/usr/bin/env python3
"""Export the CREPE neural pitch detector to a TorchScript .pt file that DW's LibTorch can load.

CREPE ships as PyTorch weights inside the `torchcrepe` package; this traces the model to a
self-contained TorchScript module (same kind of file as the `demucs.pt` we already use for stem
separation) so the C++ side can run it with no Python at runtime.

WHAT YOU NEED TO DO
-------------------
1. Have Python 3 with PyTorch and torchcrepe installed. If not:
       python3 -m pip install torch torchcrepe
   (Use a PyTorch 2.x — ideally 2.5.x — so the exported file loads in DW's LibTorch 2.5.1.)
2. Run this script:
       python3 tools/export_crepe_torchscript.py
   By default it writes crepe_full.pt and crepe_tiny.pt next to demucs.pt in the
   "Neuracoust Stem Magic" folder, so DW's build can bundle it just like the Demucs model.
3. Tell me it's done — I'll add the neural pitch-detector helper that uses the file.

Options:
    --capacity {full,tiny,both}   which model(s) to export (default: both)
    --out DIR                     output directory (default: the Stem Magic folder)
"""
import argparse
import os
import sys


def export_one(capacity: str, out_dir: str) -> str:
    import torch
    import torchcrepe

    # The pretrained weights ship inside the torchcrepe package (assets/full.pth, assets/tiny.pth).
    model = torchcrepe.Crepe(capacity)
    weights = os.path.join(os.path.dirname(torchcrepe.__file__), "assets", capacity + ".pth")
    if not os.path.isfile(weights):
        raise FileNotFoundError(f"torchcrepe weights not found at {weights}")
    model.load_state_dict(torch.load(weights, map_location="cpu"))
    model.eval()

    # CREPE takes frames of 1024 samples (audio at 16 kHz), returns a 360-bin pitch activation.
    example = torch.zeros(8, 1024, dtype=torch.float32)
    with torch.no_grad():
        try:
            traced = torch.jit.trace(model, example)
        except Exception as trace_err:  # a CNN should trace, but fall back to scripting just in case
            print(f"  trace failed ({trace_err}); trying torch.jit.script …")
            traced = torch.jit.script(model)

    out_path = os.path.join(out_dir, f"crepe_{capacity}.pt")
    traced.save(out_path)

    # Sanity check: reload the saved file and confirm the output shape is [n, 360].
    reloaded = torch.jit.load(out_path, map_location="cpu")
    with torch.no_grad():
        out = reloaded(torch.zeros(4, 1024))
    assert tuple(out.shape) == (4, 360), f"unexpected output shape {tuple(out.shape)} (expected (4, 360))"

    size_mb = os.path.getsize(out_path) / (1024 * 1024)
    print(f"  ✓ {out_path}  ({size_mb:.1f} MB, output {tuple(out.shape)})")
    return out_path


def main() -> int:
    default_out = "/Volumes/Program Dev/Neuracoust Stem Magic"
    ap = argparse.ArgumentParser(description="Export CREPE to TorchScript for DW.")
    ap.add_argument("--capacity", choices=["full", "tiny", "both"], default="both")
    ap.add_argument("--out", default=default_out, help="output directory (default: the Stem Magic folder)")
    args = ap.parse_args()

    try:
        import torch  # noqa: F401
        import torchcrepe  # noqa: F401
    except ImportError:
        print("PyTorch / torchcrepe not installed. Run:\n    python3 -m pip install torch torchcrepe", file=sys.stderr)
        return 2

    os.makedirs(args.out, exist_ok=True)
    caps = ["full", "tiny"] if args.capacity == "both" else [args.capacity]
    print(f"Exporting CREPE → TorchScript into: {args.out}")
    for cap in caps:
        print(f"[{cap}]")
        try:
            export_one(cap, args.out)
        except Exception as e:
            print(f"  ✗ failed: {e}", file=sys.stderr)
            return 1

    # The frequency mapping DW's helper needs to turn the 360-bin activation into Hz (documented here
    # so the C++ side matches CREPE exactly): cents[i] = 20*i + 1997.3794084376191, freq = 10 * 2^(cents/1200).
    print("\nDone. CREPE bin→frequency mapping for the helper:")
    print("  cents[i] = 20*i + 1997.3794084376191   (i = 0..359)")
    print("  freq_hz  = 10 * 2^(cents/1200)         → ~32.70 Hz (C1) up to ~1975.5 Hz (B6)")
    print("  input    = audio @ 16000 Hz, frames of 1024 samples, each frame zero-mean/unit-std")
    print("\nNext: tell me the .pt files exist and I'll wire up the neural pitch-detector helper.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
