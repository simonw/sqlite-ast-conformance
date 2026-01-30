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

# Patch the amalgamation to add AST capture hook
$(PATCHED): $(SQLITE_SRC) | $(BUILD_DIR)
	sed '/SelectDest dest = {SRT_Output, 0, 0, 0, 0, 0, 0};/i\  ast_capture_hook((void*)yymsp[0].minor.yy555);' \
		$(SQLITE_SRC) > $(PATCHED)

# Build the dump_ast tool
$(DUMP_AST): dump_ast.c $(PATCHED) | $(BUILD_DIR)
	gcc $(CFLAGS) -I$(BUILD_DIR) -o $(DUMP_AST) dump_ast.c -lm -lpthread

clean:
	rm -rf $(BUILD_DIR)

test: $(DUMP_AST)
	uv run pytest tests/ -v
