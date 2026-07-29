#!/usr/bin/env python3
"""
Patch the SQLite amalgamation to insert AST capture hooks.

Reads sqlite-src/sqlite3.c and writes build/sqlite3_patched.c with three
insertions:

1. In the grammar action for "cmd ::= select(X)": capture the Select*
   before sqlite3Select() runs (SELECT statement AST capture).

2. At the top of sqlite3StartTable(): record the syntactic details of a
   CREATE TABLE statement that the Table struct does not retain --
   TEMP/TEMPORARY, IF NOT EXISTS, and any schema qualifier on the name.

3. In sqlite3EndTable(), immediately after pParse->pNewTable is validated:
   capture the fully-populated Table* (plus the Select* for the
   CREATE TABLE ... AS SELECT form and the tabOpts bits for
   WITHOUT ROWID / STRICT) before codegen and before the STRICT /
   WITHOUT ROWID post-processing mutates the struct.

Each insertion is keyed to an anchor that must appear exactly once in the
amalgamation; the script fails loudly if an anchor is missing or ambiguous,
so an incompatible SQLite version bump is caught at build time rather than
producing a silently unpatched binary.

Usage: python3 patch_amalgamation.py <input sqlite3.c> <output patched.c>
"""

import sys

# Each patch is (name, anchor, insertion, where).  The anchor must occur
# exactly once.  "before" inserts the new text on its own line above the
# anchor line; "after" inserts below it.
PATCHES = [
    (
        "select capture hook (cmd ::= select action)",
        "  SelectDest dest = {SRT_Output, 0, 0, 0, 0, 0, 0};\n",
        "  ast_capture_hook((void*)yymsp[0].minor.yy555, (void*)pParse);\n",
        "before",
    ),
    (
        "start-table hook (sqlite3StartTable)",
        "  if( db->init.busy && db->init.newTnum==1 ){\n",
        "  ast_start_table_hook((void*)pParse, (const void*)pName1,"
        " (const void*)pName2, isTemp, isView, isVirtual, noErr);\n",
        "before",
    ),
    (
        "end-table hook (sqlite3EndTable)",
        "  if( pEnd==0 && pSelect==0 ){\n"
        "    return;\n"
        "  }\n"
        "  p = pParse->pNewTable;\n"
        "  if( p==0 ) return;\n",
        "  ast_end_table_hook((void*)pParse, (void*)p, tabOpts,"
        " (void*)pSelect);\n",
        "after",
    ),
]


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path, encoding="utf-8") as f:
        text = f.read()

    for name, anchor, insertion, where in PATCHES:
        count = text.count(anchor)
        if count != 1:
            print(
                f"ERROR: anchor for {name} found {count} times "
                f"(expected exactly 1).\n"
                f"Anchor:\n{anchor}\n"
                f"The SQLite amalgamation has probably changed; "
                f"update patch_amalgamation.py.",
                file=sys.stderr,
            )
            sys.exit(1)
        replacement = (
            insertion + anchor if where == "before" else anchor + insertion
        )
        text = text.replace(anchor, replacement)
        print(f"  patched: {name}")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"  wrote {out_path}")


if __name__ == "__main__":
    main()
