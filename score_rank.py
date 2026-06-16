#!/usr/bin/env python3
"""Rank multiple CSV result files and compute weighted scores per case."""

from __future__ import annotations

import argparse
import csv
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path


METRICS = ("and_count", "lev_count", "runtime_sec", "extra_peak_rss_mb")
REQUIRED_COLUMNS = ("case",) + METRICS
WEIGHTS = {
    "and_count": Decimal("0.5"),
    "lev_count": Decimal("0.2"),
    "runtime_sec": Decimal("0.2"),
    "extra_peak_rss_mb": Decimal("0.1"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Score and rank CSV result files. Each input CSV must contain "
            "case,and_count,lev_count,runtime_sec,extra_peak_rss_mb."
        )
    )
    parser.add_argument("csv_files", nargs="+", help="Input CSV result files.")
    parser.add_argument(
        "--detail-out",
        default="case_scores.csv",
        help="Output detail CSV path. Default: case_scores.csv",
    )
    parser.add_argument(
        "--summary-out",
        default="summary_rank.csv",
        help="Output summary CSV path. Default: summary_rank.csv",
    )
    return parser.parse_args()


def format_decimal(value: Decimal) -> str:
    normalized = value.normalize()
    if normalized == normalized.to_integral():
        return str(normalized.quantize(Decimal("1")))
    return format(normalized, "f")


def score_from_rank(rank: int) -> Decimal:
    return Decimal(max(11 - rank, 5))


def competition_ranks(items: list[tuple[str, Decimal]], reverse: bool = False) -> dict[str, int]:
    sorted_items = sorted(items, key=lambda item: (item[1], item[0]), reverse=reverse)
    ranks: dict[str, int] = {}
    previous_value: Decimal | None = None
    current_rank = 0

    for index, (name, value) in enumerate(sorted_items, start=1):
        if previous_value is None or value != previous_value:
            current_rank = index
            previous_value = value
        ranks[name] = current_rank

    return ranks


def read_result_csv(path: Path) -> dict[str, dict[str, Decimal]]:
    try:
        handle = path.open(newline="", encoding="utf-8-sig")
    except OSError as exc:
        raise ValueError(f"{path}: cannot open file: {exc}") from exc

    with handle:
        reader = csv.reader(handle)
        header: list[str] | None = None
        header_line_number = 0

        for line_number, row in enumerate(reader, start=1):
            if not row or all(not cell.strip() for cell in row):
                continue
            if row[0].lstrip().startswith("#"):
                continue
            header = [cell.strip() for cell in row]
            header_line_number = line_number
            break

        if header is None:
            raise ValueError(f"{path}: empty CSV or missing header")

        missing = [column for column in REQUIRED_COLUMNS if column not in header]
        if missing:
            raise ValueError(f"{path}: missing required columns: {', '.join(missing)}")
        column_indexes = {column: header.index(column) for column in REQUIRED_COLUMNS}

        def get_cell(row: list[str], column: str) -> str:
            index = column_indexes[column]
            return row[index].strip() if index < len(row) else ""

        rows: dict[str, dict[str, Decimal]] = {}
        for row_number, row in enumerate(reader, start=header_line_number + 1):
            if not row or all(not cell.strip() for cell in row):
                if rows:
                    break
                continue
            if row[0].lstrip().startswith("#"):
                continue

            case = get_cell(row, "case")
            if not case:
                raise ValueError(f"{path}:{row_number}: empty case")
            if case in rows:
                raise ValueError(f"{path}:{row_number}: duplicate case: {case}")

            metric_values: dict[str, Decimal] = {}
            for metric in METRICS:
                raw_value = get_cell(row, metric)
                try:
                    metric_values[metric] = Decimal(raw_value)
                except InvalidOperation as exc:
                    raise ValueError(
                        f"{path}:{row_number}: invalid numeric value for {metric}: {raw_value!r}"
                    ) from exc

            rows[case] = metric_values

    if not rows:
        raise ValueError(f"{path}: no data rows")

    return rows


def load_all(paths: list[Path]) -> dict[str, dict[str, dict[str, Decimal]]]:
    result_by_file: dict[str, dict[str, dict[str, Decimal]]] = {}
    seen_names: set[str] = set()

    for path in paths:
        source_name = path.name
        if source_name in seen_names:
            raise ValueError(
                f"duplicate input file name {source_name!r}; use unique basenames for scoring"
            )
        seen_names.add(source_name)
        result_by_file[source_name] = read_result_csv(path)

    expected_cases: set[str] | None = None
    expected_source = ""
    for source_name, rows in result_by_file.items():
        case_names = set(rows)
        if expected_cases is None:
            expected_cases = case_names
            expected_source = source_name
            continue

        missing = sorted(expected_cases - case_names)
        extra = sorted(case_names - expected_cases)
        if missing or extra:
            messages = []
            if missing:
                messages.append(f"missing cases compared with {expected_source}: {', '.join(missing)}")
            if extra:
                messages.append(f"extra cases compared with {expected_source}: {', '.join(extra)}")
            raise ValueError(f"{source_name}: case set mismatch; " + "; ".join(messages))

    return result_by_file


def compute_scores(
    result_by_file: dict[str, dict[str, dict[str, Decimal]]]
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    source_names = sorted(result_by_file)
    case_names = sorted(next(iter(result_by_file.values())))
    totals = {source_name: Decimal("0") for source_name in source_names}
    detail_rows: list[dict[str, object]] = []

    for case in case_names:
        metric_ranks: dict[str, dict[str, int]] = {}
        metric_scores: dict[str, dict[str, Decimal]] = {}

        for metric in METRICS:
            values = [
                (source_name, result_by_file[source_name][case][metric])
                for source_name in source_names
            ]
            ranks = competition_ranks(values)
            metric_ranks[metric] = ranks
            metric_scores[metric] = {
                source_name: score_from_rank(rank) for source_name, rank in ranks.items()
            }

        for source_name in source_names:
            weighted_score = sum(
                metric_scores[metric][source_name] * WEIGHTS[metric] for metric in METRICS
            )
            totals[source_name] += weighted_score

            row: dict[str, object] = {
                "source_csv": source_name,
                "case": case,
            }
            for metric in METRICS:
                row[metric] = result_by_file[source_name][case][metric]
            for metric in METRICS:
                row[f"{metric}_rank"] = metric_ranks[metric][source_name]
            for metric in METRICS:
                row[f"{metric}_score"] = metric_scores[metric][source_name]
            row["case_weighted_score"] = weighted_score
            detail_rows.append(row)

    final_ranks = competition_ranks(list(totals.items()), reverse=True)
    summary_rows = [
        {
            "source_csv": source_name,
            "total_score": totals[source_name],
            "final_rank": final_ranks[source_name],
        }
        for source_name in source_names
    ]
    summary_rows.sort(key=lambda row: (int(row["final_rank"]), str(row["source_csv"])))

    return detail_rows, summary_rows


def write_csv(path: Path, fieldnames: list[str], rows: list[dict[str, object]]) -> None:
    try:
        handle = path.open("w", newline="", encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"{path}: cannot write file: {exc}") from exc

    with handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            formatted = {
                key: format_decimal(value) if isinstance(value, Decimal) else value
                for key, value in row.items()
            }
            writer.writerow(formatted)


def main() -> int:
    args = parse_args()
    input_paths = [Path(path) for path in args.csv_files]

    try:
        result_by_file = load_all(input_paths)
        detail_rows, summary_rows = compute_scores(result_by_file)

        detail_fields = (
            ["source_csv", "case"]
            + list(METRICS)
            + [f"{metric}_rank" for metric in METRICS]
            + [f"{metric}_score" for metric in METRICS]
            + ["case_weighted_score"]
        )
        summary_fields = ["source_csv", "total_score", "final_rank"]

        write_csv(Path(args.detail_out), detail_fields, detail_rows)
        write_csv(Path(args.summary_out), summary_fields, summary_rows)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"wrote detail CSV: {args.detail_out}")
    print(f"wrote summary CSV: {args.summary_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
