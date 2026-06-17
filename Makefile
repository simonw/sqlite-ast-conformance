SQLITE_SRC = sqlite-src/sqlite3.c
SQLITE_HDR = sqlite-src/sqlite3.h
BUILD_DIR = build
PATCHED = $(BUILD_DIR)/sqlite3_patched.c
DUMP_AST = $(BUILD_DIR)/dump_ast

CFLAGS = -O2 -D_GNU_SOURCE -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION

.PHONY: all clean test

all: $(DUMP_AST)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Detect GNU sed (gsed on macOS, sed on Linux)
SED := $(shell command -v gsed 2>/dev/null || echo sed)

# Patch the amalgamation to add AST capture hooks:
#   - SELECT:       before the SelectDest in the "cmd ::= select" action
#   - CREATE TABLE: at the top of sqlite3StartTable() (name/TEMP/IF NOT
#                   EXISTS) and sqlite3EndTable() (the finished Table)
$(PATCHED): $(SQLITE_SRC) | $(BUILD_DIR)
	$(SED) \
		-e '/SelectDest dest = {SRT_Output, 0, 0, 0, 0, 0, 0};/i\  ast_capture_hook((void*)yymsp[0].minor.yy555);' \
		-e '/int iDb;         \/\* Database number to create the table in \*\//a\  ast_capture_table_start(pParse, pName1, pName2, isTemp, isView, isVirtual, noErr);' \
		-e '/Table \*p;                 \/\* The new table \*\//a\  ast_capture_table_end(pParse, tabOpts, pSelect);' \
		$(SQLITE_SRC) > $(PATCHED)

# Build the dump_ast tool
$(DUMP_AST): dump_ast.c $(PATCHED) | $(BUILD_DIR)
	gcc $(CFLAGS) -I$(BUILD_DIR) -o $(DUMP_AST) dump_ast.c -lm -lpthread

clean:
	rm -rf $(BUILD_DIR)

test: $(DUMP_AST)
	uv run pytest tests/ -v
