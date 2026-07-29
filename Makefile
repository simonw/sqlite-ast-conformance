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

# Patch the amalgamation to capture complete VALUES rows before SQLite compiles
# them into VDBE bytecode, then capture the completed top-level SELECT. Ignore
# SELECTs parsed internally while SQLite is loading the schema.
$(PATCHED): $(SQLITE_SRC) | $(BUILD_DIR)
	awk 'BEGIN { foundSelect=0; foundValues=0; foundMValues=0 } \
		/case [0-9]+: \/\* values ::= VALUES LP nexprlist RP \*\// { \
			inValues=1 \
		} \
		inValues && /sqlite3SelectNew\(pParse,.*SF_Values,0\);/ { \
			print; \
			lhs=$$0; \
			sub(/[[:space:]]*=.*/, "", lhs); \
			sub(/^[[:space:]]*/, "", lhs); \
			print "  ast_values_first_hook((void*)pParse, (void*)" lhs ");"; \
			foundValues=1; \
			inValues=0; \
			next \
		} \
		/case [0-9]+: \/\* mvalues ::= values COMMA LP nexprlist RP \*\// { \
			inMValues=1 \
		} \
		inMValues && /sqlite3MultiValues\(pParse,/ { \
			sub(/sqlite3MultiValues/, "ast_values_append_hook"); \
			foundMValues=1; \
			inMValues=0 \
		} \
		/SelectDest dest = \{SRT_Output, 0, 0, 0, 0, 0, 0\};/ { \
			print "  if( pParse->db->init.busy==0 ) ast_capture_hook((void*)yymsp[0].minor.yy555);"; \
			foundSelect=1 \
		} \
		{ print } \
		END { \
			if( !foundSelect || !foundValues || !foundMValues ) exit 1 \
		}' \
		$(SQLITE_SRC) > $(PATCHED)

# Build the dump_ast tool
$(DUMP_AST): dump_ast.c $(PATCHED) | $(BUILD_DIR)
	gcc $(CFLAGS) -I$(BUILD_DIR) -o $(DUMP_AST) dump_ast.c -lm -lpthread

clean:
	rm -rf $(BUILD_DIR)

test: $(DUMP_AST)
	uv run pytest tests/ -v
