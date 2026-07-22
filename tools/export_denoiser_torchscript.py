#!/usr/bin/env python3
"""Export a pretrained neural speech/vocal denoiser to a TorchScript .pt file DW's LibTorch can load.

We use Facebook's `denoiser` (Defossez et al., "Real Time Speech Enhancement in the Waveform Domain",
a.k.a. the DNS models). It is a pure waveform-domain nn.Module — input [B,1,T] → output [B,1,T] at
16 kHz — so it traces to a self-contained TorchScript module with no Python/Rust at runtime, exactly
like the demucs.pt / crepe_full.pt we already bundle. MIT-licensed, so it can ship.

The model runs at 16 kHz (band-limited to 8 kHz). DW's neuracoust_denoiser helper therefore BAND-SPLITS:
it cleans the sub-8 kHz band with this model and passes the original >8 kHz band through, so full-band
music keeps its highs while the hum/hiss below 8 kHz is removed. (DeepFilterNet's native-48 kHz model is
a future quality upgrade; its Rust STFT/ERB pipeline does not TorchScript self-contained, so it is not v1.)

WHAT YOU NEED TO DO
-------------------
1. Have Python 3 with PyTorch (2.x). Nothing else — the model is pulled from torch.hub.
2. Run this script:
       python3 tools/export_denoiser_torchscript.py
   By default it writes denoiser.pt next to demucs.pt in the "Neuracoust Stem Magic" folder, so DW's
   build bundles it just like the other models.
3. Tell me it's done — the helper + bridge + UI are already wired to use it.

Options:
    --variant {master64,dns64,dns48}   which pretrained model (default: master64, best quality)
    --out DIR                          output directory (default: the Stem Magic folder)
"""
import argparse
import os
import sys


def export(variant: str, out_dir: str) -> str:
    import torch

    # Pretrained DNS denoiser from torch.hub (facebookresearch/denoiser). Pure waveform nn.Module.
    model = torch.hub.load("facebookresearch/denoiser", variant, trust_repo=True)
    model.eval()
    sr = int(getattr(model, "sample_rate", 16000))

    # forward expects [batch, 1, samples] at the model rate; trace on a 1 s mono example.
    example = torch.zeros(1, 1, sr, dtype=torch.float32)
    with torch.no_grad():
        traced = torch.jit.trace(model, example)

    out_path = os.path.join(out_dir, "denoiser.pt")
    traced.save(out_path)

    # Sanity: reload the saved file and confirm it maps [1,1,N] → [1,1,N].
    reloaded = torch.jit.load(out_path, map_location="cpu")
    with torch.no_grad():
        out = reloaded(torch.zeros(1, 1, sr))
    assert out.dim() == 3 and out.shape[0] == 1 and out.shape[1] == 1, f"unexpected output shape {tuple(out.shape)}"

    size_mb = os.path.getsize(out_path) / (1024 * 1024)
    print(f"  ✓ {out_path}  ({size_mb:.1f} MB, sample_rate {sr} Hz, {variant})")
    return out_path


def main() -> int:
    default_out = "/Volumes/Program Dev/Neuracoust Stem Magic"
    ap = argparse.ArgumentParser(description="Export a neural denoiser to TorchScript for DW.")
    ap.add_argument("--variant", choices=["master64", "dns64", "dns48"], default="master64")
    ap.add_argument("--out", default=default_out, help="output directory (default: the Stem Magic folder)")
    args = ap.parse_args()

    try:
        import torch  # noqa: F401
    except ImportError:
        print("PyTorch not installed. Run:\n    python3 -m pip install torch", file=sys.stderr)
        return 2

    os.makedirs(args.out, exist_ok=True)
    print(f"Exporting denoiser ({args.variant}) → TorchScript into: {args.out}")
    try:
        export(args.variant, args.out)
    except Exception as e:  # noqa: BLE001
        print(f"  ✗ failed: {e}", file=sys.stderr)
        return 1

    print("\nDone. The denoiser runs at 16 kHz; DW's helper band-splits (clean <8 kHz, pass >8 kHz).")
    print("Next: rebuild DW — CMake bundles denoiser.pt and the 노이즈 제거 menu item comes alive.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
