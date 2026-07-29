# sqlite-ast-conformance

A language-independent conformance suite for implementations of a SQLite SQL parser, covering SELECT and CREATE TABLE statements.

The `sqlite_ast_conformance/ast-tests/` directory contains JSON files, each defining a SQL statement and its expected abstract syntax tree (AST). These test fixtures are generated using the **official SQLite parser** so they represent ground truth for how SQLite parses these statements.

The package is available on PyPI, so you can install it and access the test fixtures programmatically:

```bash
pip install sqlite-ast-conformance
```

```python
from sqlite_ast_conformance import AST_TESTS_DIR

for test_file in sorted(AST_TESTS_DIR.glob("*.json")):
    print(test_file.name)
```

## How it works

The ASTs represent the **raw parse tree** produced by SQLite's Lemon parser, captured *before* any name resolution or `SELECT *` expansion. This means:

- `SELECT *` produces `{"type": "star"}` — no schema knowledge needed
- `SELECT foo.bar` produces a `dot` node with `name` children — no table lookups
- All tests run against an in-memory database with no tables

There are two capture points:

- **SELECT**: the grammar action for `cmd ::= select` captures the `Select*` before `sqlite3Select()` modifies it.
- **CREATE TABLE**: SQLite's grammar never builds a freestanding AST for CREATE TABLE — the parse actions incrementally populate `pParse->pNewTable` (a `Table*`). A hook at the top of `sqlite3EndTable()` captures that structure once it is fully populated, before code generation and before STRICT / WITHOUT ROWID post-processing mutates it. A second hook in `sqlite3StartTable()` records syntax that the `Table` struct does not retain (`TEMP`, `IF NOT EXISTS`, the schema qualifier).

The in-memory database is "bootstrapped" (a table is created and dropped in both the main and temp schemas) before capture is enabled. A truly empty database has schema file format 1, in which SQLite ignores `DESC` on `PRIMARY KEY`/`UNIQUE` constraint columns; real databases are file format 4, so the fixtures match real-database behavior.

## Test file format

Each JSON file in `sqlite_ast_conformance/ast-tests/` has two keys:

```json
{
  "sql": "SELECT 1 + 2",
  "ast": {
    "type": "select",
    "distinct": false,
    "all": false,
    "columns": [
      {
        "expr": {
          "type": "binary",
          "op": "+",
          "left": {"type": "integer", "value": 1},
          "right": {"type": "integer", "value": 2}
        },
        "alias": null
      }
    ],
    "from": null,
    "where": null,
    "group_by": null,
    "having": null,
    "order_by": null,
    "limit": null
  }
}
```

## Building and running the reference tests

### Prerequisites

- GCC (or compatible C compiler)
- Python 3.10+ and [uv](https://docs.astral.sh/uv/)
- Git

### 1. Clone the SQLite source

```bash
git clone --depth 1 https://github.com/sqlite/sqlite.git sqlite-src
```

### 2. Build the SQLite amalgamation

```bash
cd sqlite-src
./configure
make sqlite3.c
cd ..
```

### 3. Build the `dump_ast` tool

```bash
make
```

This runs `patch_amalgamation.py`, which patches the SQLite amalgamation to insert the AST capture hooks (the `cmd ::= select` grammar action, `sqlite3StartTable`, and `sqlite3EndTable`), then compiles `dump_ast.c` which includes the patched amalgamation and provides a JSON serializer for the AST. Each patch is keyed to an anchor that must appear exactly once in the amalgamation, so an incompatible SQLite version bump fails the build loudly instead of producing an unpatched binary.

### 4. Run the conformance tests

```bash
uv run pytest -v
```

This runs `test_ast.py` which loads every JSON file from `sqlite_ast_conformance/ast-tests/`, calls `dump_ast` with the SQL, and compares the output to the expected AST.

Note that `test_ast.py` is a check that `dump_ast` built from the current SQLite source still reproduces the committed fixtures — it validates the fixture-generation toolchain, not your parser.

### 5. Try individual queries

```bash
./build/dump_ast "SELECT * FROM foo WHERE x > 5 ORDER BY y"
./build/dump_ast "CREATE TABLE t (a INTEGER PRIMARY KEY, b TEXT CHECK (b <> ''))"
```

## Generating new test fixtures

```bash
python generate_test.py <name> "<sql>"
# Example:
python generate_test.py my_test "SELECT a, b FROM t WHERE a > 1"
```

This creates `sqlite_ast_conformance/ast-tests/my_test.json` using `dump_ast` to generate the expected AST.

## AST node types

### Expressions

| Type | Description | Key fields |
|------|-------------|------------|
| `integer` | Integer literal | `value` |
| `float` | Float literal | `value` (string) |
| `string` | String literal | `value` |
| `blob` | Blob literal | `value` |
| `null` | NULL | — |
| `boolean` | TRUE/FALSE | `value` |
| `name` | Identifier | `name` |
| `star` | Wildcard `*` | — |
| `dot` | Qualified name `a.b` | `left`, `right` |
| `binary` | Binary operator | `op`, `left`, `right` |
| `unary` | Unary operator | `op`, `operand` |
| `function` | Function call | `name`, `args`, `distinct`, optional `over` |
| `cast` | CAST expression | `expr`, `as` |
| `case` | CASE expression | `operand`, `when_clauses`, `else` |
| `between` | BETWEEN | `expr`, `low`, `high` |
| `in` | IN | `expr`, `values` or `select` |
| `exists` | EXISTS | `select` |
| `subquery` | Scalar subquery | `select` |
| `collate` | COLLATE | `expr`, `collation` |
| `isnull` | IS NULL | `operand` |
| `notnull` | IS NOT NULL | `operand` |
| `truth_test` | IS TRUE/FALSE | `op`, `operand` |
| `parameter` | Bind parameter | `name` |

### SELECT

```
type: "select"
├── distinct: bool
├── all: bool
├── with: [...CTEs...]
├── columns: [{expr, alias}, ...]
├── from: [{type: "table"/"subquery", ...}, ...]
├── where: expr
├── group_by: [expr, ...]
├── having: expr
├── window_definitions: [...]
├── order_by: [{expr, direction, nulls}, ...]
├── limit: expr
└── offset: expr
```

Compound selects (`UNION`, `INTERSECT`, `EXCEPT`) use `type: "compound"` with a `body` array.

Multi-row `VALUES` clauses made entirely of constants are converted to a co-routine during parsing: the AST becomes `SELECT * FROM (subquery)` where the subquery holds only the **first** row, marked with `"via_coroutine": true` and a `"rows"` count. The remaining rows are code-generated immediately and are not retained in the parse tree.

### CREATE TABLE

```
type: "create_table"
├── name: str
├── schema: str | null        (explicit qualifier only, e.g. "main.t")
├── temp: bool                (TEMP/TEMPORARY keyword)
├── if_not_exists: bool
├── columns: [column, ...]    (null for the AS SELECT form)
├── primary_key: {...} | null
├── unique: [{...}, ...]
├── checks: [{name, expr}, ...] | null
├── foreign_keys: [{...}, ...]
├── without_rowid: bool
├── strict: bool
└── as_select: select | null
```

Each column:

```
├── name: str
├── type: str | null          (declared type; canonical uppercase for standard types)
├── affinity: "TEXT"/"NUMERIC"/"INTEGER"/"REAL"/"BLOB"/"FLEXNUM"/"NONE"
├── not_null: bool
├── not_null_on_conflict: str | null   ("ROLLBACK"/"ABORT"/"FAIL"/"IGNORE"/"REPLACE")
├── default: expr | null      (a span node wrapping the parsed value)
├── collate: str | null
├── primary_key: bool         (column mentioned in any PRIMARY KEY clause)
├── unique: bool              (column-level UNIQUE or PRIMARY KEY)
└── generated: {expr, stored: bool} | null
```

`primary_key` is one of two shapes, matching SQLite's own two representations:

- An `INTEGER PRIMARY KEY` (rowid alias) has `"integer_primary_key": true`, a single-column `columns` list (name only), `autoincrement`, and `on_conflict`.
- Any other PRIMARY KEY becomes a parse-time index: `"integer_primary_key": false`, `columns` entries with `name`, `collation`, and `direction` (`ASC`/`DESC`), plus `on_conflict` and the internal `index_name` (`sqlite_autoindex_<table>_<n>`).

Each `unique` entry has the same index shape: `columns` (with `name`, `collation`, `direction`), `on_conflict`, and `index_name`. Entries appear in declaration order.

Each `foreign_keys` entry:

```
├── columns: [str, ...]                (child columns)
├── references: {table, columns}      (columns null = parent's PRIMARY KEY)
├── on_delete: "NO ACTION"/"RESTRICT"/"SET NULL"/"SET DEFAULT"/"CASCADE"
├── on_update: (same values)
└── deferred: bool
```

### CREATE TABLE parse-time normalizations

Like the SELECT fixtures, the CREATE TABLE fixtures encode what SQLite's parser *actually retains*, quirks included:

- **Column-level vs table-level constraints are not distinguished.** `a INT UNIQUE` and `UNIQUE (a)` both become index entries; column-level and table-level CHECKs are merged into one ordered list. The syntactic distinction is lost.
- **Every CHECK constraint has a name**: the `CONSTRAINT` name if given, otherwise the source text of the check expression.
- **CONSTRAINT names can leak.** The parser only resets the pending constraint name at commas *between table-level constraints*, so an unnamed table-level CHECK that directly follows a named column constraint inherits its name (see the `create_table_check_name_leak` fixture; stock SQLite reports the same name in constraint-violation errors).
- **Standard type names are canonicalized** to uppercase `INT`, `INTEGER`, `REAL`, `TEXT`, `BLOB`, `ANY`; any other declared type keeps its raw source text (e.g. `VARCHAR(10)`).
- **`INT PRIMARY KEY` is not a rowid alias** — only the exact type `INTEGER` (single-column, ascending) sets `integer_primary_key`; `INT PRIMARY KEY` and `INTEGER PRIMARY KEY DESC` become ordinary PK indexes.
- **DEFAULT values are span nodes**: SQLite stores both the original source text and the parsed expression.
- **A bare column reference in a generated column** (`b AS (a)`) is wrapped in a unary `+`.
- **Foreign key constraint names are discarded**, and a `MATCH` clause (and `ON INSERT`) is parsed but not retained. `ON DELETE NO ACTION` is indistinguishable from having no action clause.
- **Deferrability is a single bool**: only `DEFERRABLE INITIALLY DEFERRED` produces `"deferred": true`; `DEFERRABLE`, `DEFERRABLE INITIALLY IMMEDIATE` and `NOT DEFERRABLE` are all `false`.
- **CHECK expressions get the same expression-level rewrites as SELECT**: `LIKE`/`GLOB` become function calls with reversed argument order (plus an optional third ESCAPE argument), `x AND 0` constant-folds, etc.
- **Subqueries and bind parameters are rejected** by SQLite inside CHECK constraints, so no fixtures exist for them (`dump_ast` exits non-zero).

## Using these tests in your own parser

To test your own SQLite parser implementation:

1. Read each JSON file from `sqlite_ast_conformance/ast-tests/`
2. Parse the `sql` field with your parser
3. Compare your AST output against the `ast` field
4. The exact JSON structure must match — field names, nesting, and values

The test fixtures are pure JSON with no dependencies, so they can be consumed by any programming language.
