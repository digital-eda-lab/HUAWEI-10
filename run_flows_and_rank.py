#!/usr/bin/env python3
"""Run multiple ABC flows, store result CSVs, and rank all stored results."""

from __future__ import annotations

import argparse
import csv
import os
import sys
from pathlib import Path

import score_rank
from run_tc_public_abc import (
    DEFAULT_ABC,
    DEFAULT_ABC_RC,
    DEFAULT_CASE_DIR,
    RESULT_COLUMNS,
    discover_cases,
    load_flow,
    measure_abc_baseline_rss,
    parse_case_numbers,
    run_case,
    select_cases,
)


ROOT = Path(__file__).resolve().parent
DEFAULT_DATA_DIR = ROOT / "datacsv"
DEFAULT_DETAIL_OUT = DEFAULT_DATA_DIR / "case_scores.csv"
DEFAULT_SUMMARY_OUT = DEFAULT_DATA_DIR / "summary_rank.csv"
RANK_OUTPUT_NAMES = {DEFAULT_DETAIL_OUT.name, DEFAULT_SUMMARY_OUT.name}
FLOW_BEGIN = "# FLOW_BEGIN"
FLOW_END = "# FLOW_END"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("flows", nargs="+", type=Path, help="ABC flow files to run")
    parser.add_argument("--abc", type=Path, default=DEFAULT_ABC, help="ABC executable path")
    parser.add_argument("--abc-rc", type=Path, default=DEFAULT_ABC_RC, help="ABC rc file for aliases")
    parser.add_argument("--case-dir", type=Path, default=DEFAULT_CASE_DIR, help="tc_public directory")
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR, help="directory for result CSVs")
    parser.add_argument("--cases", help="case numbers to run, e.g. 1 or 1,3,26 or 1-5,26")
    parser.add_argument("--timeout", type=int, default=None, help="timeout seconds for each case")
    parser.add_argument("--limit", type=int, default=None, help="run only the first N cases")
    parser.add_argument(
        "--detail-out",
        type=Path,
        default=DEFAULT_DETAIL_OUT,
        help=f"detail ranking CSV path. Default: {DEFAULT_DETAIL_OUT}",
    )
    parser.add_argument(
        "--summary-out",
        type=Path,
        default=DEFAULT_SUMMARY_OUT,
        help=f"summary ranking CSV path. Default: {DEFAULT_SUMMARY_OUT}",
    )
    return parser.parse_args()


def resolve_optional(path: Path | None) -> Path | None:
    return path.resolve() if path is not None else None


def output_path_for_flow(data_dir: Path, flow: Path) -> Path:
    return data_dir / f"{flow.stem}.csv"


def validate_inputs(args: argparse.Namespace) -> tuple[Path, Path | None, Path, list[Path], list[Path]]:
    abc = args.abc.resolve()
    abc_rc = resolve_optional(args.abc_rc)
    case_dir = args.case_dir.resolve()
    data_dir = args.data_dir.resolve()
    flows = [flow.resolve() for flow in args.flows]

    if not abc.is_file() or not os.access(abc, os.X_OK):
        raise FileNotFoundError(f"ABC executable not found or not executable: {abc}")
    if abc_rc is not None and not abc_rc.is_file():
        raise FileNotFoundError(f"ABC rc file not found: {abc_rc}")

    for flow in flows:
        if not flow.is_file():
            raise FileNotFoundError(f"ABC flow script not found: {flow}")

    output_paths = [output_path_for_flow(data_dir, flow) for flow in flows]
    seen_outputs: set[str] = set()
    for output_path in output_paths:
        if output_path.name in seen_outputs:
            raise ValueError(f"duplicate generated CSV name: {output_path.name}")
        seen_outputs.add(output_path.name)
        if output_path.exists():
            raise FileExistsError(f"result CSV already exists: {output_path}")

    return abc, abc_rc, case_dir, flows, output_paths


def select_requested_cases(case_dir: Path, cases_text: str | None, limit: int | None) -> list[Path]:
    cases = select_cases(case_dir, parse_case_numbers(cases_text)) if cases_text else discover_cases(case_dir)
    if limit is not None:
        if limit <= 0:
            raise ValueError("--limit must be greater than 0")
        cases = cases[:limit]
    if not cases:
        raise RuntimeError("No case was selected")
    return cases


def write_results_csv_with_flow(
    output_path: Path,
    results: list[tuple[str, int, int, float, float]],
    flow_path: Path,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    flow_text = flow_path.read_text(encoding="utf-8")

    with output_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(RESULT_COLUMNS)
        for case_name, and_count, lev_count, runtime_sec, extra_peak_rss_mb in results:
            writer.writerow([case_name, and_count, lev_count, f"{runtime_sec:.6f}", f"{extra_peak_rss_mb:.3f}"])

        csv_file.write("\n")
        csv_file.write(f"{FLOW_BEGIN}\n")
        for line in flow_text.splitlines():
            csv_file.write(f"# {line}\n")
        csv_file.write(f"{FLOW_END}\n")


def run_flow(
    abc: Path,
    abc_rc: Path | None,
    flow: Path,
    cases: list[Path],
    baseline_rss_mb: float,
    timeout: int | None,
) -> list[tuple[str, int, int, float, float]]:
    read_cmds, body_cmds = load_flow(flow)
    results: list[tuple[str, int, int, float, float]] = []

    print(f"Flow: {flow}")
    for index, blif in enumerate(cases, start=1):
        case_name = blif.parent.name
        print(f"[{index}/{len(cases)}] running {case_name} ... ", end="", flush=True)
        and_count, lev_count, runtime_sec, peak_rss_mb = run_case(
            abc, abc_rc, read_cmds, body_cmds, blif, timeout
        )
        extra_peak_rss_mb = max(0.0, peak_rss_mb - baseline_rss_mb)
        print(
            f"AND={and_count} lev={lev_count} runtime_sec={runtime_sec:.6f} "
            f"extra_peak_rss_mb={extra_peak_rss_mb:.3f}",
            flush=True,
        )
        results.append((case_name, and_count, lev_count, runtime_sec, extra_peak_rss_mb))

    return results


def first_data_header(path: Path) -> list[str] | None:
    try:
        handle = path.open(newline="", encoding="utf-8-sig")
    except OSError as exc:
        raise ValueError(f"{path}: cannot open file: {exc}") from exc

    with handle:
        reader = csv.reader(handle)
        for row in reader:
            if not row or all(not cell.strip() for cell in row):
                continue
            if row[0].lstrip().startswith("#"):
                continue
            return [cell.strip() for cell in row]
    return None


def looks_like_result_csv(path: Path) -> bool:
    header = first_data_header(path)
    return header is not None and all(column in header for column in score_rank.REQUIRED_COLUMNS)


def discover_result_csvs(data_dir: Path, excluded_paths: set[Path]) -> list[Path]:
    result_paths: list[Path] = []
    for path in sorted(data_dir.glob("*.csv")):
        resolved = path.resolve()
        if resolved in excluded_paths or path.name in RANK_OUTPUT_NAMES:
            continue
        if looks_like_result_csv(resolved):
            result_paths.append(resolved)
    if not result_paths:
        raise ValueError(f"no result CSV files found under {data_dir}")
    return result_paths


def write_rank_outputs(result_paths: list[Path], detail_out: Path, summary_out: Path) -> None:
    result_by_file = score_rank.load_all(result_paths)
    detail_rows, summary_rows = score_rank.compute_scores(result_by_file)

    detail_fields = (
        ["source_csv", "case"]
        + list(score_rank.METRICS)
        + [f"{metric}_rank" for metric in score_rank.METRICS]
        + [f"{metric}_score" for metric in score_rank.METRICS]
        + ["case_weighted_score"]
    )
    summary_fields = ["source_csv", "total_score", "final_rank"]

    detail_out.parent.mkdir(parents=True, exist_ok=True)
    summary_out.parent.mkdir(parents=True, exist_ok=True)
    score_rank.write_csv(detail_out, detail_fields, detail_rows)
    score_rank.write_csv(summary_out, summary_fields, summary_rows)


def main() -> int:
    args = parse_args()

    try:
        abc, abc_rc, case_dir, flows, output_paths = validate_inputs(args)
        cases = select_requested_cases(case_dir, args.cases, args.limit)
        detail_out = args.detail_out.resolve()
        summary_out = args.summary_out.resolve()

        print(f"ABC: {abc}")
        print(f"ABC rc: {abc_rc}")
        print(f"Cases: {len(cases)}")
        baseline_rss_mb = measure_abc_baseline_rss(abc, abc_rc, args.timeout)
        print(f"ABC baseline peak_rss_mb: {baseline_rss_mb:.3f}")

        for flow, output_path in zip(flows, output_paths):
            results = run_flow(abc, abc_rc, flow, cases, baseline_rss_mb, args.timeout)
            if not results:
                raise RuntimeError(f"No case was run for {flow}")
            write_results_csv_with_flow(output_path, results, flow)
            print(f"CSV written: {output_path}")

        excluded_paths = {detail_out, summary_out}
        result_paths = discover_result_csvs(output_paths[0].parent, excluded_paths)
        write_rank_outputs(result_paths, detail_out, summary_out)

        print(f"ranked CSV files: {len(result_paths)}")
        print(f"wrote detail CSV: {detail_out}")
        print(f"wrote summary CSV: {summary_out}")
    except (FileNotFoundError, FileExistsError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
