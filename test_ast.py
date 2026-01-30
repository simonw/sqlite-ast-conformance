"""
Conformance tests for SQLite SELECT query AST parsing.

Loads JSON test files from ast-tests/ and verifies that the dump_ast tool
(which uses the official SQLite parser) produces the expected AST for each
SQL query.
"""

import json
import subprocess
import sys
from pathlib import Path

import pytest

# Path to the dump_ast binary
DUMP_AST = Path(__file__).parent / "build" / "dump_ast"
AST_TESTS_DIR = Path(__file__).parent / "ast-tests"


def load_test_cases():
    """Load all JSON test files from ast-tests/."""
    cases = []
    for path in sorted(AST_TESTS_DIR.glob("*.json")):
        with open(path) as f:
            data = json.load(f)
        cases.append(pytest.param(data["sql"], data["ast"], id=path.stem))
    return cases


@pytest.mark.parametrize("sql, expected_ast", load_test_cases())
def test_select_ast(sql, expected_ast):
    """Verify that parsing the SQL produces the expected AST."""
    result = subprocess.run(
        [str(DUMP_AST), sql],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 0, (
        f"dump_ast failed for: {sql}\nstderr: {result.stderr}"
    )
    actual_ast = json.loads(result.stdout)
    assert actual_ast == expected_ast, (
        f"AST mismatch for: {sql}\n"
        f"Expected:\n{json.dumps(expected_ast, indent=2)}\n"
        f"Actual:\n{json.dumps(actual_ast, indent=2)}"
    )
