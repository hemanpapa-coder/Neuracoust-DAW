#!/bin/zsh
set -euo pipefail

support_root="${1:?Application Support path is required}"
repo_path="${support_root}/seed-vc"
venv_path="${support_root}/.venv"
repo_url="https://github.com/Plachtaa/seed-vc.git"

mkdir -p "${support_root}"

if [[ ! -f "${repo_path}/inference.py" ]]; then
  echo "Seed-VC 소스 내려받는 중…"
  /usr/bin/git clone --depth 1 "${repo_url}" "${repo_path}"
else
  echo "Seed-VC 소스 확인 완료"
fi

python_candidates=(
  "/Users/hasnagm4pro/.local/opt/python@3.10/bin/python3.10"
  "/opt/homebrew/bin/python3.10"
  "/usr/local/bin/python3.10"
)
python_bin=""
for candidate in "${python_candidates[@]}"; do
  if [[ -x "${candidate}" ]]; then
    python_bin="${candidate}"
    break
  fi
done

if [[ -z "${python_bin}" ]]; then
  uv_bin="/Users/hasnagm4pro/.local/bin/uv"
  [[ -x "${uv_bin}" ]] || uv_bin="/opt/homebrew/bin/uv"
  if [[ -x "${uv_bin}" ]]; then
    echo "Python 3.10 전용 런타임 내려받는 중…"
    "${uv_bin}" python install 3.10
    python_bin="$("${uv_bin}" python find 3.10)"
  else
    echo "Python 3.10이 없어 Homebrew로 설치합니다…"
    brew_bin="/Users/hasnagm4pro/.local/bin/brew"
    [[ -x "${brew_bin}" ]] || brew_bin="/opt/homebrew/bin/brew"
    [[ -x "${brew_bin}" ]] || { echo "Homebrew 또는 uv를 찾지 못했습니다."; exit 2; }
    "${brew_bin}" install python@3.10
    python_bin="$("${brew_bin}" --prefix python@3.10)/bin/python3.10"
  fi
fi

if [[ ! -x "${venv_path}/bin/python" ]]; then
  echo "전용 Python 환경 만드는 중…"
  "${python_bin}" -m venv "${venv_path}"
fi

echo "Seed-VC 구성 요소 설치 중…"
"${venv_path}/bin/python" -m pip install --upgrade pip wheel setuptools
"${venv_path}/bin/python" -m pip install -r "${repo_path}/requirements-mac.txt"

echo "Apple Silicon 호환 패치 확인 중…"
if /usr/bin/grep -q 'torch.from_numpy(F0_ori).to(device)' "${repo_path}/inference.py"; then
  /usr/bin/sed -i '' \
    's/torch.from_numpy(F0_ori).to(device)/torch.from_numpy(F0_ori).float().to(device)/' \
    "${repo_path}/inference.py"
  /usr/bin/sed -i '' \
    's/torch.from_numpy(F0_alt).to(device)/torch.from_numpy(F0_alt).float().to(device)/' \
    "${repo_path}/inference.py"
fi
if /usr/bin/grep -q 'torch.from_numpy(f0s).to(self.device)' "${repo_path}/modules/rmvpe.py"; then
  /usr/bin/sed -i '' \
    's/torch.from_numpy(f0s).to(self.device)/torch.from_numpy(f0s).float().to(self.device)/' \
    "${repo_path}/modules/rmvpe.py"
fi
if ! /usr/bin/grep -q '^import soundfile as sf' "${repo_path}/inference.py"; then
  /usr/bin/sed -i '' '/^import librosa$/a\
import soundfile as sf
' "${repo_path}/inference.py"
fi
if /usr/bin/grep -q 'torchaudio.save(os.path.join(args.output' "${repo_path}/inference.py"; then
  "${venv_path}/bin/python" - "${repo_path}/inference.py" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = 'torchaudio.save(os.path.join(args.output, f"vc_{source_name}_{target_name}_{length_adjust}_{diffusion_steps}_{inference_cfg_rate}.wav"), vc_wave.cpu(), sr)'
new = 'sf.write(os.path.join(args.output, f"vc_{source_name}_{target_name}_{length_adjust}_{diffusion_steps}_{inference_cfg_rate}.wav"), vc_wave.cpu().numpy().T, sr, subtype="PCM_24")'
path.write_text(text.replace(old, new))
PY
fi

echo "설치 완료"
