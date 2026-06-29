#!/usr/bin/env python3
"""Run an ABC flow for tc_public cases and print per-case metrics."""

from __future__ import annotations

import argparse
import csv
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
DEFAULT_ABC = ROOT / "abc" / "abc"
DEFAULT_ABC_RC = ROOT / "abc" / "abc.rc"
DEFAULT_CASE_DIR = ROOT / "tc_public"
DEFAULT_FLOW = ROOT / "experiments" / "resyn_stage1" / "scripts" / "baseline.abc"

ABC_ERROR_RE = re.compile(r"^\*\* cmd error:", re.MULTILINE)
PS_RE = re.compile(
    r"i/o\s*=\s*(?P<pi>\d+)\s*/\s*(?P<po>\d+).*?"
    r"and\s*=\s*(?P<and>\d+).*?"
    r"lev\s*=\s*(?P<lev>\d+)",
    re.IGNORECASE,
)
RESULT_COLUMNS = ["case", "and_count", "lev_count", "runtime_sec", "extra_peak_rss_mb"]


def case_sort_key(path: Path) -> tuple[int, str]:
    match = re.search(r"tc_public_(\d+)$", path.parent.name)
    return (int(match.group(1)) if match else sys.maxsize, path.parent.name)


def discover_cases(case_dir: Path) -> list[Path]:
    cases = sorted(case_dir.glob("tc_public_*/output.blif"), key=case_sort_key)
    if not cases:
        raise FileNotFoundError(f"No cases found under {case_dir}/tc_public_*/output.blif")
    return cases


def parse_case_numbers(text: str) -> list[int]:
    numbers: list[int] = []
    seen: set[int] = set()

    for part in text.split(","):
        item = part.strip()
        if not item:
            continue

        if "-" in item:
            start_text, end_text = item.split("-", 1)
            if not start_text.isdigit() or not end_text.isdigit():
                raise ValueError(f"Invalid case range: {item}")
            start = int(start_text)
            end = int(end_text)
            if start > end:
                raise ValueError(f"Invalid descending case range: {item}")
            values = range(start, end + 1)
        else:
            if not item.isdigit():
                raise ValueError(f"Invalid case number: {item}")
            values = [int(item)]

        for number in values:
            if number < 1 or number > 30:
                raise ValueError(f"Case number out of range 1-30: {number}")
            if number not in seen:
                numbers.append(number)
                seen.add(number)

    if not numbers:
        raise ValueError("--cases did not contain any case number")
    return numbers


def select_cases(case_dir: Path, case_numbers: list[int]) -> list[Path]:
    cases = []
    missing = []
    for number in case_numbers:
        path = case_dir / f"tc_public_{number}" / "output.blif"
        if path.is_file():
            cases.append(path)
        else:
            missing.append(str(path))

    if missing:
        raise FileNotFoundError("Missing selected case files:\n" + "\n".join(missing))
    return cases


def load_flow(flow_path: Path) -> tuple[list[str], list[str]]:
    read_cmds: list[str] = []
    body_cmds: list[str] = []

    for raw_line in flow_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        lowered = line.lower()
        if lowered.startswith(("read_blif ", "read_aiger ", "read_bench ", "read_verilog ", "read ")):
            continue
        if lowered.startswith("read_lib "):
            read_cmds.append(line)
        else:
            body_cmds.append(line)

    if not body_cmds:
        raise ValueError(f"Flow script has no executable commands: {flow_path}")
    return read_cmds, body_cmds


def parse_final_stats(stdout: str) -> tuple[int, int]:
    matches = list(PS_RE.finditer(stdout))
    if not matches:
        raise ValueError("ABC output does not contain a parseable ps/print_stats line")

    match = matches[-1]
    return int(match.group("and")), int(match.group("lev"))


def run_abc_commands(
    abc: Path,
    commands: list[str],
    script_name: str,
    timeout: int | None,
) -> tuple[int, bool, str, str, float, float]:
    with tempfile.TemporaryDirectory(prefix="tc_public_abc_") as tmp_dir:
        script_path = Path(tmp_dir) / f"{script_name}.abc"
        stdout_path = Path(tmp_dir) / f"{script_name}.stdout.log"
        stderr_path = Path(tmp_dir) / f"{script_name}.stderr.log"
        script_path.write_text("\n".join(commands) + "\n", encoding="utf-8")

        start = time.perf_counter()
        timed_out = False
        with stdout_path.open("w", encoding="utf-8", errors="replace") as stdout_file, stderr_path.open(
            "w", encoding="utf-8", errors="replace"
        ) as stderr_file:
            proc = subprocess.Popen(
                [str(abc), "-f", str(script_path)],
                cwd=ROOT,
                text=True,
                stdout=stdout_file,
                stderr=stderr_file,
                start_new_session=True,
            )

            while True:
                waited_pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
                if waited_pid == proc.pid:
                    break

                if timeout is not None and time.perf_counter() - start > timeout:
                    timed_out = True
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    waited_pid, status, usage = os.wait4(proc.pid, 0)
                    break

                time.sleep(0.05)

        runtime_sec = time.perf_counter() - start
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")

    if os.WIFSIGNALED(status):
        returncode = -os.WTERMSIG(status)
    elif os.WIFEXITED(status):
        returncode = os.WEXITSTATUS(status)
    else:
        returncode = 124 if timed_out else 1

    peak_rss_kb = usage.ru_maxrss
    if sys.platform == "darwin":
        peak_rss_kb /= 1024
    peak_rss_mb = peak_rss_kb / 1024
    return returncode, timed_out, stdout, stderr, runtime_sec, peak_rss_mb


def measure_abc_baseline_rss(abc: Path, abc_rc: Path | None, timeout: int | None) -> float:
    commands = []
    if abc_rc is not None:
        commands.append(f"source {abc_rc}")
    commands.append("quit")

    returncode, timed_out, stdout, stderr, _, peak_rss_mb = run_abc_commands(
        abc, commands, "abc_empty_baseline", timeout
    )
    if returncode != 0 or timed_out or ABC_ERROR_RE.search(stderr):
        timeout_text = "timed out\n" if timed_out else ""
        raise RuntimeError(
            f"ABC baseline run failed, return code {returncode}\n"
            f"{timeout_text}"
            f"stderr:\n{stderr[-2000:]}\n"
            f"stdout:\n{stdout[-2000:]}"
        )
    return peak_rss_mb


def run_case(
    abc: Path,
    abc_rc: Path | None,
    read_cmds: list[str],
    body_cmds: list[str],
    blif: Path,
    timeout: int | None,
) -> tuple[int, int, float, float]:
    commands = []
    if abc_rc is not None:
        commands.append(f"source {abc_rc}")
    commands.extend([*read_cmds, f"read_blif {blif}", *body_cmds])
    if not any(cmd.lower() in {"ps", "print_stats"} for cmd in body_cmds):
        commands.append("print_stats")

    returncode, timed_out, stdout, stderr, runtime_sec, peak_rss_mb = run_abc_commands(
        abc, commands, blif.parent.name, timeout
    )

    if returncode != 0 or timed_out or ABC_ERROR_RE.search(stderr):
        timeout_text = "timed out\n" if timed_out else ""
        raise RuntimeError(
            f"ABC failed for {blif.parent.name}, return code {returncode}\n"
            f"{timeout_text}"
            f"stderr:\n{stderr[-2000:]}\n"
            f"stdout:\n{stdout[-2000:]}"
        )

    and_count, lev_count = parse_final_stats(stdout)
    return and_count, lev_count, runtime_sec, peak_rss_mb


def write_results_csv(output_path: Path, results: list[tuple[str, int, int, float, float]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(RESULT_COLUMNS)
        for case_name, and_count, lev_count, runtime_sec, extra_peak_rss_mb in results:
            writer.writerow([case_name, and_count, lev_count, f"{runtime_sec:.6f}", f"{extra_peak_rss_mb:.3f}"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abc", type=Path, default=DEFAULT_ABC, help="ABC executable path")
    parser.add_argument("--abc-rc", type=Path, default=DEFAULT_ABC_RC, help="ABC rc file for aliases")
    parser.add_argument("--case-dir", type=Path, default=DEFAULT_CASE_DIR, help="tc_public directory")
    parser.add_argument("--flow", type=Path, default=DEFAULT_FLOW, help="ABC script to run for every case")
    parser.add_argument("--cases", help="case numbers to run, e.g. 1 or 1,3,26 or 1-5,26")
    parser.add_argument("--timeout", type=int, default=None, help="timeout seconds for each case")
    parser.add_argument("--limit", type=int, default=None, help="run only the first N cases")
    parser.add_argument("--output", type=Path, help="write the final result table to this CSV file")
    args = parser.parse_args()

    abc = args.abc.resolve()
    abc_rc = args.abc_rc.resolve() if args.abc_rc else None
    case_dir = args.case_dir.resolve()
    flow = args.flow.resolve()

    if not abc.is_file() or not os.access(abc, os.X_OK):
        raise FileNotFoundError(f"ABC executable not found or not executable: {abc}")
    if abc_rc is not None and not abc_rc.is_file():
        raise FileNotFoundError(f"ABC rc file not found: {abc_rc}")
    if not flow.is_file():
        raise FileNotFoundError(f"ABC flow script not found: {flow}")

    read_cmds, body_cmds = load_flow(flow)
    cases = select_cases(case_dir, parse_case_numbers(args.cases)) if args.cases else discover_cases(case_dir)
    if args.limit is not None:
        if args.limit <= 0:
            raise ValueError("--limit must be greater than 0")
        cases = cases[: args.limit]
    results: list[tuple[str, int, int, float, float]] = []
    baseline_rss_mb = measure_abc_baseline_rss(abc, abc_rc, args.timeout)

    print(f"ABC: {abc}")
    print(f"ABC rc: {abc_rc}")
    print(f"Flow: {flow}")
    print(f"Cases: {len(cases)}")
    print(f"ABC baseline peak_rss_mb: {baseline_rss_mb:.3f}")

    for index, blif in enumerate(cases, start=1):
        case_name = blif.parent.name
        print(f"[{index}/{len(cases)}] running {case_name} ... ", end="", flush=True)
        and_count, lev_count, runtime_sec, peak_rss_mb = run_case(
            abc, abc_rc, read_cmds, body_cmds, blif, args.timeout
        )
        extra_peak_rss_mb = max(0.0, peak_rss_mb - baseline_rss_mb)
        print(
            f"AND={and_count} lev={lev_count} runtime_sec={runtime_sec:.6f} "
            f"extra_peak_rss_mb={extra_peak_rss_mb:.3f}",
            flush=True,
        )
        results.append((case_name, and_count, lev_count, runtime_sec, extra_peak_rss_mb))

    if not results:
        raise RuntimeError("No case was run")

    print(",".join(RESULT_COLUMNS))
    for case_name, and_count, lev_count, runtime_sec, extra_peak_rss_mb in results:
        print(f"{case_name},{and_count},{lev_count},{runtime_sec:.6f},{extra_peak_rss_mb:.3f}")

    if args.output:
        output = args.output.resolve()
        write_results_csv(output, results)
        print(f"CSV written: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
