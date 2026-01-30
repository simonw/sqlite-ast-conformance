#!/usr/bin/env python3
"""
Generate a JSON test file for a given SQL query using dump_ast.

Usage: python generate_test.py <filename> <sql>
  Creates ast-tests/<filename>.json with {"sql": ..., "ast": ...}

Or:     python generate_test.py --batch
  Reads (filename, sql) pairs from stdin, one JSON object per line:
  {"name": "select_literal", "sql": "SELECT 1"}
"""

import json
import subprocess
import sys
from pathlib import Path

DUMP_AST = Path(__file__).parent / "build" / "dump_ast"
AST_TESTS_DIR = Path(__file__).parent / "ast-tests"


def generate_one(name: str, sql: str) -> bool:
    """Generate a single test file. Returns True on success."""
    result = subprocess.run(
        [str(DUMP_AST), sql],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode != 0:
        print(f"FAIL {name}: {result.stderr.strip()}", file=sys.stderr)
        return False

    try:
        ast = json.loads(result.stdout)
    except json.JSONDecodeError as e:
        print(f"FAIL {name}: invalid JSON: {e}", file=sys.stderr)
        return False

    out = AST_TESTS_DIR / f"{name}.json"
    with open(out, "w") as f:
        json.dump({"sql": sql, "ast": ast}, f, indent=2)
        f.write("\n")

    print(f"  OK {name}: {sql}")
    return True


def main():
    if len(sys.argv) == 3 and sys.argv[1] != "--batch":
        name, sql = sys.argv[1], sys.argv[2]
        if not generate_one(name, sql):
            sys.exit(1)
    elif "--batch" in sys.argv:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            generate_one(obj["name"], obj["sql"])
    else:
        print("Usage: generate_test.py <name> <sql>", file=sys.stderr)
        print("       generate_test.py --batch < input.jsonl", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
