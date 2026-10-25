"""Load cases/cases.json into a pandas DataFrame, one row per case, indexed by case id.

`bugs` (comma-separated in the file) and `withhold` become lists; cases without
`difficulty` get an empty string. Run directly to print the table or save it as CSV.
"""

import argparse
import json
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[1]
CASES_PATH = REPO_ROOT / "cases" / "cases.json"

COLUMNS = [
    "id",
    "title",
    "setup",
    "customer",
    "use_case",
    "bugs",
    "difficulty",
    "withhold",
    "report",
    "rev",
    "rev_good",
]


def split_bugs(value) -> list[str]:
    if isinstance(value, list):
        return [bug for bug in value if bug]
    return [bug.strip() for bug in str(value or "").split(",") if bug.strip()]


def load_cases(path: Path = CASES_PATH) -> pd.DataFrame:
    cases = json.loads(Path(path).read_text(encoding="utf-8"))
    df = pd.DataFrame(cases).reindex(columns=COLUMNS)
    df["bugs"] = df["bugs"].map(split_bugs)
    df["withhold"] = df["withhold"].map(lambda value: value if isinstance(value, list) else [])
    df["difficulty"] = df["difficulty"].fillna("")
    df["bug_count"] = df["bugs"].map(len)
    return df.set_index("id")


def main() -> int:
    parser = argparse.ArgumentParser(description="Turn cases.json into a pandas table.")
    parser.add_argument("--cases", type=Path, default=CASES_PATH, help="Path to cases.json")
    parser.add_argument("--csv", type=Path, help="Also write the table to this CSV file")
    args = parser.parse_args()

    df = load_cases(args.cases)
    with pd.option_context("display.max_columns", None, "display.width", 200, "display.max_colwidth", 40):
        print(df.drop(columns=["report"]))
    if args.csv:
        df.to_csv(args.csv)
        print(f"wrote {len(df)} cases to {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
