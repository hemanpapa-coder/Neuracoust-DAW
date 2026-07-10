#!/usr/bin/env python3
import argparse
import csv
import json
import math
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

sf = None


def require_soundfile():
    global sf
    if sf is not None:
        return sf
    try:
        import soundfile as soundfile_module
    except Exception as exc:
        print(f"soundfile import failed: {exc}", file=sys.stderr)
        sys.exit(2)
    sf = soundfile_module
    return sf


@dataclass
class Plugin:
    name: str
    brand: str
    bundle: str
    cid: str
    class_name: str
    category: str
    loadable: str


@dataclass
class Parameter:
    parameter_id: int
    default_normalized: float
    plain: str
    flags: int
    title: str


def plugin_key(plugin):
    return "|".join([plugin.name, plugin.class_name, plugin.cid, plugin.bundle])


def shell_command(parts):
    return " ".join(shlex.quote(str(part)) for part in parts if str(part))


def build_resume_command(args, out_dir, next_start_index):
    parts = [
        "python3",
        "tools/waves_compat_audit.py",
        "--build-dir",
        args.build_dir,
        "--input",
        args.input,
        "--input-gain-db",
        f"{args.input_gain_db:g}",
        "--out-dir",
        out_dir,
        "--filter",
        args.filter,
        "--limit",
        args.limit,
        "--param-limit",
        args.param_limit,
        "--inspect-limit",
        args.inspect_limit,
        "--start-index",
        next_start_index,
        "--resume",
    ]
    if args.include_mono:
        parts.append("--include-mono")
    if args.no_renders:
        parts.append("--no-renders")
    if args.fail_on_problem:
        parts.append("--fail-on-problem")
    if args.plugins:
        parts.append("--plugins")
        parts.extend(args.plugins)
    return shell_command(parts)


PARAMETER_KEYWORDS = re.compile(
    r"(gain|output|threshold|thresh|ceiling|atten|trim|drive|mix|dry|wet|freq|frequency|"
    r"reverb|time|ratio|range|fader|band|q|width|phase|invert|ms|stereo|duo|"
    r"bright|brighter|knob|amount|intensity)",
    re.IGNORECASE,
)


def run(command, cwd, timeout=120):
    try:
        process = subprocess.Popen(
            command,
            cwd=str(cwd),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        stdout, stderr = process.communicate(timeout=timeout)
        process.stdout = None
        process.stderr = None
        class CompletedResult:
            returncode = process.returncode
        result = CompletedResult()
        result.stdout = stdout or ""
        result.stderr = stderr or ""
        return result
    except subprocess.TimeoutExpired as exc:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except Exception:
            pass
        try:
            stdout, stderr = process.communicate(timeout=5)
        except Exception:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except Exception:
                pass
            try:
                stdout, stderr = process.communicate(timeout=5)
            except Exception:
                stdout, stderr = "", ""
        class TimeoutResult:
            returncode = 124
            stdout = stdout if isinstance(stdout, str) else (exc.stdout if isinstance(exc.stdout, str) else "")
            stderr = (stderr if isinstance(stderr, str) else (exc.stderr if isinstance(exc.stderr, str) else "")) + f"\nTimed out after {timeout}s"
        return TimeoutResult()


def db(value):
    return 20.0 * math.log10(max(float(value), 1.0e-12))


def audio_metrics(path):
    sf = require_soundfile()
    data, sample_rate = sf.read(str(path), always_2d=True)
    if data.size == 0:
        return {
            "sample_rate": sample_rate,
            "frames": 0,
            "peak_dbfs": -240.0,
            "rms_dbfs": -240.0,
            "tail_100ms_rms_dbfs": -240.0,
        }
    peak = abs(data).max()
    rms = math.sqrt(float((data ** 2).mean()))
    tail_frames = min(len(data), max(1, int(sample_rate * 0.1)))
    tail = math.sqrt(float((data[-tail_frames:] ** 2).mean()))
    return {
        "sample_rate": int(sample_rate),
        "frames": int(len(data)),
        "peak_dbfs": round(db(peak), 3),
        "rms_dbfs": round(db(rms), 3),
        "tail_100ms_rms_dbfs": round(db(tail), 3),
    }


def diff_metrics(left_path, right_path):
    sf = require_soundfile()
    left, left_sr = sf.read(str(left_path), always_2d=True)
    right, right_sr = sf.read(str(right_path), always_2d=True)
    frames = min(len(left), len(right))
    channels = min(left.shape[1], right.shape[1])
    if frames <= 0 or channels <= 0:
        return {"diff_rms_dbfs": -240.0, "max_abs_diff_dbfs": -240.0}
    diff = right[:frames, :channels] - left[:frames, :channels]
    rms = math.sqrt(float((diff ** 2).mean()))
    peak = abs(diff).max()
    return {
        "diff_rms_dbfs": round(db(rms), 3),
        "max_abs_diff_dbfs": round(db(peak), 3),
        "sample_rate_match": left_sr == right_sr,
    }


def plugin_matches_filter_text(plugin, filter_text):
    needle = (filter_text or "").strip()
    if not needle or needle.lower() == "waves":
        return True
    haystack = " ".join([
        plugin.name,
        plugin.class_name,
        plugin.category,
    ])
    try:
        if re.search(needle, haystack, re.IGNORECASE):
            return True
    except re.error:
        pass
    return any(plugin_matches_selector(plugin, selector) for selector in re.split(r"\s*\|\s*", needle) if selector)


def load_plugins(build_dir, cwd, filter_text):
    audit = build_dir / "neuracoust_vst3_host_audit"
    result = run([str(audit), "--list-only", "--filter", "Waves"], cwd, timeout=180)
    if result.returncode != 0:
        raise RuntimeError(result.stderr or result.stdout)
    plugins = []
    for row in csv.DictReader(result.stdout.splitlines(), delimiter="\t"):
        if row.get("name") == "name":
            continue
        if row.get("brand", "").lower() != "waves":
            continue
        plugins.append(
            Plugin(
                name=row.get("name", ""),
                brand=row.get("brand", ""),
                bundle=row.get("bundle", ""),
                cid=row.get("cid", ""),
                class_name=row.get("class", ""),
                category=row.get("category", ""),
                loadable=row.get("loadable", ""),
            )
        )
    return [plugin for plugin in plugins if plugin_matches_filter_text(plugin, filter_text)]


def compact_token(value):
    return re.sub(r"[^a-z0-9]+", "", (value or "").lower())


def plugin_matches_selector(plugin, selector):
    needle = (selector or "").strip().lower()
    if not needle:
        return False
    haystack = " ".join([
        plugin.name,
        plugin.class_name,
        plugin.category,
    ]).lower()
    if needle in {plugin.name.lower(), plugin.class_name.lower(), plugin.cid.lower()}:
        return True
    if len(needle) <= 2:
        words = {token for token in re.split(r"[^a-z0-9]+", haystack) if token}
        return needle in words
    if needle in haystack:
        return True
    compact_needle = compact_token(needle)
    compact_haystack = compact_token(haystack)
    if compact_needle and compact_needle in compact_haystack:
        return True
    tokens = [token for token in re.split(r"[^a-z0-9]+", needle) if token]
    words = {token for token in re.split(r"[^a-z0-9]+", haystack) if token}
    return bool(tokens) and all((token in words if len(token) <= 1 else token in haystack) for token in tokens)


def prepare_input_with_gain(input_path, out_dir, gain_db):
    if abs(gain_db) < 1.0e-9:
        return input_path
    sf = require_soundfile()
    data, sample_rate = sf.read(str(input_path), always_2d=True)
    gain = math.pow(10.0, gain_db / 20.0)
    processed = data * gain
    peak = abs(processed).max() if processed.size else 0.0
    if peak > 0.98:
        processed *= 0.98 / peak
    suffix = f"{gain_db:+.1f}dB".replace("+", "plus").replace("-", "minus").replace(".", "p")
    target = out_dir / f"{input_path.stem}_{suffix}.wav"
    sf.write(str(target), processed, sample_rate)
    return target


def make_sine_input(out_dir, sample_rate, frequency_hz, seconds=3.0, level_dbfs=-18.0):
    sf = require_soundfile()
    frames = max(1, int(sample_rate * seconds))
    amplitude = math.pow(10.0, level_dbfs / 20.0)
    data = []
    for frame in range(frames):
        sample = amplitude * math.sin(2.0 * math.pi * frequency_hz * frame / sample_rate)
        data.append((sample, sample))
    target = out_dir / f"sine_{int(frequency_hz)}Hz_{seconds:g}s_{level_dbfs:g}dBFS_stereo.wav"
    sf.write(str(target), data, sample_rate)
    return target


def stimulus_for_plugin(plugin, default_input_path, out_dir, input_metrics):
    name_lower = (plugin.name or "").lower()
    if "deesser" in name_lower or "de-esser" in name_lower or "sibilance" in name_lower:
        return make_sine_input(
            out_dir,
            int(input_metrics.get("sample_rate") or 44100),
            7000.0,
            level_dbfs=-18.0,
        ), "7kHz sine for sibilance/de-esser response"
    return default_input_path, "default input"


def inspect_parameters(build_dir, cwd, plugin, limit):
    editor = build_dir / "Neuracoust VST3 Editor Host"
    command = [
        str(editor),
        "--plugin",
        plugin.bundle,
        "--name",
        plugin.name,
        "--class-name",
        plugin.class_name or plugin.name,
        "--inspect-parameters",
        "--limit",
        str(limit),
    ]
    result = run(command, cwd, timeout=120)
    parameters = []
    if result.returncode != 0:
        return parameters, result.stderr[-2000:] or result.stdout[-2000:]
    for line in result.stdout.splitlines():
        if not line.startswith("PARAMINFO\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 8:
            continue
        try:
            parameters.append(
                Parameter(
                    parameter_id=int(parts[1]),
                    default_normalized=float(parts[3]),
                    plain=parts[4],
                    flags=int(parts[5]),
                    title=parts[6],
                )
            )
        except ValueError:
            continue
    return parameters, ""


def choose_parameters(parameters, max_count):
    scored = []
    seen = set()
    fallback = []
    for parameter in parameters:
        if parameter.parameter_id in seen:
            continue
        if parameter.flags & 1 == 0:
            continue
        title = parameter.title or ""
        if title.lower() in {"bank", "pitch bend", "after touch"}:
            continue
        if not re.match(r"^(control\s+\d+|bank select|modulation wheel|breath controller|foot controller|data entry|channel volume|balance|pan position|expression control|effect control|general control|sustain pedal|portamento|sostenuto|soft pedal|legato pedal|hold2 pedal|sound control|nrpn|rpn)\b", title, re.IGNORECASE):
            fallback.append(parameter)
        if not (PARAMETER_KEYWORDS.search(title) or re.search(r"(1\s*k|1khz|1000|800|1\.25k)", title, re.IGNORECASE)):
            continue
        seen.add(parameter.parameter_id)
        lower = title.lower()
        score = 0
        if re.search(r"(1\s*k|1khz|1000)", lower):
            score += 120
        if "band 4 gain" in lower:
            score += 110
        if "band 3" in lower and "gain" in lower:
            score += 110
        if "out gain" in lower or "in gain" in lower:
            score += 115
        if "master" in lower and ("g" in lower or "gain" in lower):
            score += 105
        if lower in {"out", "output"} or "output" in lower:
            score += 100
        if "gain" in lower:
            score += 90
        if "bright" in lower or "knob" in lower or lower in {"amount", "intensity"}:
            score += 85
        if "threshold" in lower or "thresh" in lower or "ceiling" in lower:
            score += 75
        if "dry/wet" in lower or lower == "mix" or " mix" in lower:
            score += 70
        if "freq" in lower or "hz" in lower:
            score += 45
        if "q" == lower[-1:] or " q" in lower:
            score += 10
        if "on/off" in lower or "solo" in lower:
            score -= 50
        if score <= 0:
            score = 1
        scored.append((score, len(scored), parameter))
    scored.sort(key=lambda item: (-item[0], item[1]))
    selected = [item[2] for item in scored[:max_count]]
    if selected:
        return selected
    return fallback[:max_count]


def find_parameter_id(parameters, title):
    for candidate in parameters:
        if (candidate.title or "").lower() == title.lower():
            return candidate.parameter_id
    return None


def append_on_switch(companions, parameters, title):
    parameter_id = find_parameter_id(parameters, title)
    if parameter_id is not None:
        companions.append((parameter_id, 1.0))


def companion_parameter_values(plugin, parameter, parameters):
    companions = []
    match = re.search(r"\bBand\s*(\d+)\b.*Gain", parameter.title or "", re.IGNORECASE)
    if match:
        band = match.group(1)
        on_off = re.compile(rf"\bBand\s*{re.escape(band)}\b.*On/Off", re.IGNORECASE)
        for candidate in parameters:
            if on_off.search(candidate.title or ""):
                companions.append((candidate.parameter_id, 1.0))
                break
    title = parameter.title or ""
    if re.search(r"\bHpfFreq-L\b", title, re.IGNORECASE):
        append_on_switch(companions, parameters, "HpfOn-L")
    if re.search(r"\bHpfFreq-R\b", title, re.IGNORECASE):
        append_on_switch(companions, parameters, "HpfOn-R")
    if re.search(r"\bLpfFreq-L\b", title, re.IGNORECASE):
        append_on_switch(companions, parameters, "LpfOn-L")
    if re.search(r"\bLpfFreq-R\b", title, re.IGNORECASE):
        append_on_switch(companions, parameters, "LpfOn-R")
    if title.startswith("Gate "):
        append_on_switch(companions, parameters, "Gate On")
    if title.startswith("Comp "):
        append_on_switch(companions, parameters, "Comp On")
    if title.startswith("Leveller "):
        append_on_switch(companions, parameters, "Leveller On")
    if title.startswith("DeEsser "):
        append_on_switch(companions, parameters, "DeEsser On")
    if title.startswith("Limiter "):
        append_on_switch(companions, parameters, "Limiter On")
    if "generator" in (plugin.name or "").lower() and title.lower() in {"gain", "frequency", "signal type", "phase flip"}:
        append_on_switch(companions, parameters, "On Off")
    if (plugin.name or "").lower().startswith("pse"):
        if title in {"Threshold", "Threshold Num"}:
            range_id = find_parameter_id(parameters, "Range")
            if range_id is not None:
                companions.append((range_id, 0.0))
        if title == "DuckerGain":
            append_on_switch(companions, parameters, "Ducker On/Off")
    return companions


def render(build_dir, cwd, input_path, output_path, plugin, params=None):
    worker = build_dir / "neuracoust_vst3_process_worker"
    command = [
        str(worker),
        "--plugin",
        plugin.bundle,
        "--name",
        plugin.name,
        "--input",
        str(input_path),
        "--output",
        str(output_path),
        "--class-name",
        plugin.class_name or plugin.name,
    ]
    for parameter_id, value in params or []:
        command.extend(["--param", f"{parameter_id}:{value:.6f}"])
    last_result = None
    for attempt in range(3):
        if output_path.exists():
            output_path.unlink(missing_ok=True)
        result = run(command, cwd, timeout=120)
        last_result = result
        failed_to_instantiate = (
            result.returncode != 0 and
            "No audio component class could be instantiated" in (result.stderr or result.stdout)
        )
        if result.returncode == 0 or not failed_to_instantiate:
            return result
        time.sleep(0.35 * (attempt + 1))
    return last_result


def classify_result(input_metrics, default_metrics, diff_from_input, parameter_results):
    return classify_result_for_category("", input_metrics, default_metrics, diff_from_input, parameter_results)


def classify_result_for_category(category, input_metrics, default_metrics, diff_from_input, parameter_results, name=""):
    category_lower = (category or "").lower()
    name_lower = (name or "").lower()
    if "instrument" in category_lower and default_metrics["peak_dbfs"] < -120.0:
        return "instrument-no-midi"
    if (
        "analyzer" in category_lower or
        "meter" in category_lower or
        "meter" in name_lower or
        "dorrough" in name_lower
    ) and diff_from_input["diff_rms_dbfs"] <= -120.0:
        return "meter-pass-through"
    if (
        "restoration" in category_lower or
        "feedback" in name_lower or
        "noise" in name_lower
    ) and diff_from_input["diff_rms_dbfs"] <= -120.0:
        return "content-dependent-pass-through"
    default_changed = diff_from_input["diff_rms_dbfs"] > -80.0
    param_changed = any(item.get("diff_from_default", {}).get("diff_rms_dbfs", -240.0) > -80.0 for item in parameter_results)
    if param_changed:
        return "parameter-responsive"
    if "stream receive" in name_lower and default_metrics["peak_dbfs"] < -120.0:
        return "stream-receive-no-source"
    if default_metrics["peak_dbfs"] < -120.0:
        return "silent-output"
    if not default_changed and not param_changed:
        return "no-obvious-dsp-change"
    if not param_changed:
        return "default-only-change"
    return "parameter-responsive"


def write_markdown(report, path):
    problem_statuses = {"render-failed", "silent-output", "no-obvious-dsp-change"}
    status_counts = {}
    problem_plugins = []
    for item in report.get("plugins", []):
        status = item.get("status", "unknown")
        status_counts[status] = status_counts.get(status, 0) + 1
        if status in problem_statuses:
            problem_plugins.append(item)
    lines = [
        "# Waves Compatibility Audit",
        "",
        f"- Input: `{report['input']}`",
        f"- Input gain: {report.get('input_gain_db', 0.0):+.1f} dB",
        f"- Waves plugins after filters: {report.get('filtered_plugin_count', 'unknown')}",
        f"- Batch start index: {report.get('batch_start_index', 0)}",
        f"- Batch limit: {report.get('batch_limit', 'unknown')}",
        f"- Batch completed this run: {report.get('batch_completed_this_run', 'unknown')}",
        f"- Next start index: {report.get('next_start_index', 'unknown')}",
        f"- Plugins tested: {len(report['plugins'])}",
        f"- Problem candidates: {len(problem_plugins)}",
        "",
        "## Status Counts",
        "",
    ]
    for status, count in sorted(status_counts.items()):
        lines.append(f"- {status}: {count}")
    if problem_plugins:
        lines.extend(["", "## Problem Candidates", ""])
        for item in problem_plugins:
            global_index = item.get("global_index")
            index_text = f"#{global_index} " if global_index is not None else ""
            lines.append(f"- {item.get('status', 'unknown')}: {index_text}{item.get('name', '')} ({item.get('category', '')})")
    resume_command = report.get("resume_command", "")
    if resume_command:
        lines.extend([
            "",
            "## Resume",
            "",
            "```zsh",
            resume_command,
            "```",
        ])
    lines.extend([
        "",
        "## Plugin Details",
        "",
        "| Index | Status | Plugin | Category | Stimulus | Default RMS | Default diff | Param checks |",
        "|---:|---|---|---|---|---:|---:|---:|",
    ])
    for item in report["plugins"]:
        lines.append(
            "| {index} | {status} | {name} | {category} | {stimulus} | {rms:.2f} | {diff:.2f} | {count} |".format(
                index=item.get("global_index", ""),
                status=item["status"],
                name=item["name"].replace("|", "\\|"),
                category=item["category"].replace("|", "\\|"),
                stimulus=(item.get("stimulus") or "").replace("|", "\\|"),
                rms=item.get("default_metrics", {}).get("rms_dbfs", -240.0),
                diff=item.get("diff_from_input", {}).get("diff_rms_dbfs", -240.0),
                count=len(item.get("parameter_results", [])),
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def update_report_summary(report):
    problem_statuses = {"render-failed", "silent-output", "no-obvious-dsp-change"}
    status_counts = {}
    problem_plugins = []
    for item in report.get("plugins", []):
        status = item.get("status", "unknown")
        status_counts[status] = status_counts.get(status, 0) + 1
        if status in problem_statuses:
            problem_plugins.append(item)
    report["tested_count"] = len(report.get("plugins", []))
    report["status_counts"] = dict(sorted(status_counts.items()))
    report["problem_candidate_count"] = len(problem_plugins)
    report["problem_candidates"] = [
        {
            "global_index": item.get("global_index"),
            "name": item.get("name", ""),
            "class_name": item.get("class_name", ""),
            "category": item.get("category", ""),
            "status": item.get("status", "unknown"),
        }
        for item in problem_plugins
    ]


def write_report(report, json_path, md_path):
    update_report_summary(report)
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    write_markdown(report, md_path)


def run_summary_self_test():
    report = {
        "input": "/tmp/input.wav",
        "plugins": [
            {
                "global_index": 0,
                "name": "L2 Stereo",
                "class_name": "L2 Stereo",
                "category": "Dynamics",
                "status": "parameter-responsive",
            },
            {
                "global_index": 1,
                "name": "Example Broken",
                "class_name": "Example Broken Stereo",
                "category": "EQ / Filter",
                "status": "no-obvious-dsp-change",
            },
        ],
    }
    update_report_summary(report)
    if report.get("tested_count") != 2:
        print("SUMMARY_SELF_TEST failed: tested_count", file=sys.stderr)
        return 1
    if report.get("status_counts") != {"no-obvious-dsp-change": 1, "parameter-responsive": 1}:
        print("SUMMARY_SELF_TEST failed: status_counts", file=sys.stderr)
        return 1
    if report.get("problem_candidate_count") != 1:
        print("SUMMARY_SELF_TEST failed: problem_candidate_count", file=sys.stderr)
        return 1
    candidates = report.get("problem_candidates", [])
    if len(candidates) != 1 or candidates[0].get("name") != "Example Broken":
        print("SUMMARY_SELF_TEST failed: problem_candidates", file=sys.stderr)
        return 1

    clean_report = {"input": "/tmp/input.wav", "plugins": [report["plugins"][0]]}
    update_report_summary(clean_report)
    if clean_report.get("problem_candidate_count") != 0:
        print("SUMMARY_SELF_TEST failed: clean problem count", file=sys.stderr)
        return 1
    print("SUMMARY_SELF_TEST ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Audit Waves VST3 DSP responsiveness through Neuracoust host tools.")
    parser.add_argument("--self-test-summary", action="store_true", help="Run report summary self-test and exit.")
    parser.add_argument("--build-dir", default="build/dev")
    parser.add_argument("--input", default="test_audio/1kHz_3s_minus18dBFS_stereo_44k1_16bit.wav")
    parser.add_argument("--input-gain-db", type=float, default=0.0, help="Apply gain to a temporary copy of the input before auditing.")
    parser.add_argument("--out-dir", default="runs/waves-compat-audit")
    parser.add_argument("--filter", default="Waves")
    parser.add_argument("--limit", type=int, default=24)
    parser.add_argument("--param-limit", type=int, default=3)
    parser.add_argument("--inspect-limit", type=int, default=180)
    parser.add_argument("--include-mono", action="store_true")
    parser.add_argument("--plugins", nargs="*", default=[])
    parser.add_argument("--start-index", type=int, default=0, help="Skip this many filtered plugins before auditing.")
    parser.add_argument("--resume", action="store_true", help="Keep existing output directory and append to report if present.")
    parser.add_argument("--no-renders", action="store_true", help="Delete per-plugin rendered WAV files after metrics are collected.")
    parser.add_argument("--fail-on-problem", action="store_true", help="Return non-zero when render-failed, silent-output, or no-obvious-dsp-change is found.")
    parser.add_argument("--max-seconds", type=float, default=0.0, help="Stop cleanly before starting a new plugin after this many seconds.")
    args = parser.parse_args()

    if args.self_test_summary:
        return run_summary_self_test()

    cwd = Path.cwd()
    build_dir = (cwd / args.build_dir).resolve()
    input_path = (cwd / args.input).resolve() if not Path(args.input).is_absolute() else Path(args.input)
    out_dir = (cwd / args.out_dir).resolve() if not Path(args.out_dir).is_absolute() else Path(args.out_dir)
    render_dir = out_dir / "renders"
    if out_dir.exists() and not args.resume:
        shutil.rmtree(out_dir)
    render_dir.mkdir(parents=True, exist_ok=True)
    input_path = prepare_input_with_gain(input_path, out_dir, args.input_gain_db)

    plugins = load_plugins(build_dir, cwd, args.filter)
    if args.plugins:
        plugins = [plugin for plugin in plugins if any(plugin_matches_selector(plugin, selector) for selector in args.plugins)]
    if not args.include_mono:
        plugins = [
            plugin for plugin in plugins
            if "stereo" in plugin.name.lower()
            and "5.0" not in plugin.name.lower()
            and "5.1" not in plugin.name.lower()
            and "7.1" not in plugin.name.lower()
            and "quad" not in plugin.name.lower()
            and "ambix" not in plugin.name.lower()
            and "fuma" not in plugin.name.lower()
        ]
    filtered_plugin_count = len(plugins)
    selected_plugins = []
    if args.start_index > 0:
        plugins = plugins[args.start_index:]
    plugins = plugins[: max(0, args.limit)]
    for selected_index, plugin in enumerate(plugins):
        selected_plugins.append((args.start_index + selected_index, plugin))

    input_metrics = audio_metrics(input_path)
    json_path = out_dir / "report.json"
    md_path = out_dir / "report.md"
    if args.resume and json_path.exists():
        try:
            report = json.loads(json_path.read_text(encoding="utf-8"))
        except Exception:
            report = {"input": str(input_path), "input_gain_db": args.input_gain_db, "input_metrics": input_metrics, "plugins": []}
    else:
        report = {"input": str(input_path), "input_gain_db": args.input_gain_db, "input_metrics": input_metrics, "plugins": []}
    completed_keys = {item.get("plugin_key") for item in report.get("plugins", []) if item.get("plugin_key")}
    completed_names = {item.get("name") for item in report.get("plugins", []) if item.get("name")}
    report["input"] = str(input_path)
    report["input_gain_db"] = args.input_gain_db
    report["input_metrics"] = input_metrics
    report["filtered_plugin_count"] = filtered_plugin_count
    report["batch_start_index"] = args.start_index
    report["batch_limit"] = args.limit
    report["batch_requested_count"] = len(selected_plugins)
    report["audit_options"] = {
        "filter": args.filter,
        "include_mono": args.include_mono,
        "plugins": args.plugins,
        "param_limit": args.param_limit,
        "inspect_limit": args.inspect_limit,
        "no_renders": args.no_renders,
        "max_seconds": args.max_seconds,
    }
    deadline = time.monotonic() + args.max_seconds if args.max_seconds > 0.0 else None
    batch_completed_this_run = 0
    last_global_index = args.start_index - 1

    for index, (global_index, plugin) in enumerate(selected_plugins, start=1):
        if deadline is not None and batch_completed_this_run > 0 and time.monotonic() >= deadline:
            print(f"Stopping at time limit before plugin index {global_index}.", flush=True)
            break
        key = plugin_key(plugin)
        if args.resume and (key in completed_keys or plugin.name in completed_names):
            last_global_index = max(last_global_index, global_index)
            continue
        safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", plugin.name).strip("_")
        plugin_dir = render_dir / f"{global_index + 1:03d}_{safe_name}"
        plugin_dir.mkdir(parents=True, exist_ok=True)
        default_output = plugin_dir / "default.wav"
        plugin_input_path, stimulus = stimulus_for_plugin(plugin, input_path, out_dir, input_metrics)
        plugin_input_metrics = audio_metrics(plugin_input_path)
        item = {
            "global_index": global_index,
            "plugin_key": key,
            "name": plugin.name,
            "class_name": plugin.class_name,
            "category": plugin.category,
            "bundle": plugin.bundle,
            "cid": plugin.cid,
            "stimulus": stimulus,
            "input": str(plugin_input_path),
            "input_metrics": plugin_input_metrics,
            "parameter_results": [],
        }
        print(f"[{index}/{len(selected_plugins)} global={global_index}] {plugin.name}", flush=True)
        rendered = render(build_dir, cwd, plugin_input_path, default_output, plugin)
        item["render_returncode"] = rendered.returncode
        item["render_stdout_tail"] = rendered.stdout[-1200:]
        item["render_stderr_tail"] = rendered.stderr[-1200:]
        if rendered.returncode != 0 or not default_output.exists():
            item["status"] = "render-failed"
            report["plugins"].append(item)
            batch_completed_this_run += 1
            last_global_index = max(last_global_index, global_index)
            report["batch_completed_this_run"] = batch_completed_this_run
            report["next_start_index"] = min(filtered_plugin_count, last_global_index + 1)
            report["resume_command"] = build_resume_command(args, out_dir, report["next_start_index"])
            write_report(report, json_path, md_path)
            continue
        item["default_metrics"] = audio_metrics(default_output)
        item["diff_from_input"] = diff_metrics(plugin_input_path, default_output)

        parameters, inspect_error = inspect_parameters(build_dir, cwd, plugin, args.inspect_limit)
        item["inspect_error"] = inspect_error
        item["parameter_count_seen"] = len(parameters)
        for parameter in choose_parameters(parameters, args.param_limit):
            value = 0.0 if parameter.default_normalized > 0.5 else 1.0
            param_output = plugin_dir / f"param_{parameter.parameter_id}_{safe_name}.wav"
            parameter_values = companion_parameter_values(plugin, parameter, parameters) + [(parameter.parameter_id, value)]
            param_render = render(build_dir, cwd, plugin_input_path, param_output, plugin, parameter_values)
            param_item = {
                "parameter_id": parameter.parameter_id,
                "title": parameter.title,
                "default_normalized": parameter.default_normalized,
                "test_value": value,
                "parameter_values": parameter_values,
                "returncode": param_render.returncode,
                "stderr_tail": param_render.stderr[-800:],
            }
            if param_render.returncode == 0 and param_output.exists():
                param_item["metrics"] = audio_metrics(param_output)
                param_item["diff_from_default"] = diff_metrics(default_output, param_output)
                if args.no_renders:
                    param_output.unlink(missing_ok=True)
            item["parameter_results"].append(param_item)
        item["status"] = classify_result_for_category(
            plugin.category,
            plugin_input_metrics,
            item["default_metrics"],
            item["diff_from_input"],
            item["parameter_results"],
            plugin.name,
        )
        report["plugins"].append(item)
        batch_completed_this_run += 1
        last_global_index = max(last_global_index, global_index)
        report["batch_completed_this_run"] = batch_completed_this_run
        report["next_start_index"] = min(filtered_plugin_count, last_global_index + 1)
        report["resume_command"] = build_resume_command(args, out_dir, report["next_start_index"])
        if args.no_renders:
            default_output.unlink(missing_ok=True)
            try:
                plugin_dir.rmdir()
            except OSError:
                pass
        write_report(report, json_path, md_path)

    report["batch_completed_this_run"] = batch_completed_this_run
    report["next_start_index"] = min(filtered_plugin_count, last_global_index + 1)
    report["resume_command"] = build_resume_command(args, out_dir, report["next_start_index"])
    write_report(report, json_path, md_path)
    print(f"Wrote {json_path}")
    print(f"Wrote {md_path}")
    if args.fail_on_problem:
        problem_statuses = {"render-failed", "silent-output", "no-obvious-dsp-change"}
        if any(item.get("status") in problem_statuses for item in report.get("plugins", [])):
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
