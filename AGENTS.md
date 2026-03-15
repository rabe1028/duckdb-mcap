# AGENTS.md

## Project Overview

DuckDB extension for reading MCAP files (robotics log format). Decodes CDR-serialized ROS2 messages into native DuckDB types.

## Architecture

```
src/
  mcap_extension.cpp          # Extension entry point, table functions, CDR→DuckDB Value conversion
  include/
    mcap_extension.hpp         # Extension class declaration
    cdr_deserializer.hpp       # CDR binary reader, ROS2 msg schema parser, type helpers
third_party/
  mcap/include/mcap/           # foxglove/mcap header-only C++ library
  lz4/                         # LZ4 decompression (source)
tools/
  generate_test_mcap.cpp       # Test data generator (Autoware-like MCAP files)
data/
  demo.mcap                    # Uncompressed test file (450 messages, 5 topics)
  demo_zstd.mcap               # zstd-compressed test file
  demo_lz4.mcap                # LZ4-compressed test file
test/sql/
  mcap.test                    # SQL logic tests
```

## Key Design Decisions

- **zstd**: Uses DuckDB's bundled zstd (`duckdb_zstd` namespace) with `using namespace` directive, avoiding duplicate library
- **LZ4**: Bundled separately because DuckDB's lz4 lacks frame API (`lz4frame.h`)
- **LogicalType constants**: Must use `LogicalType(LogicalTypeId::XXX)` instead of `LogicalType::XXX` to avoid GCC 14 multiple-definition linker errors in static builds
- **CDR reader safety**: All read methods set `valid_=false` on buffer overrun to prevent infinite loops
- **Byte arrays**: `uint8[]`/`byte[]` fields map to `BLOB` instead of `LIST(UTINYINT)` for performance

## Build & Test

```bash
git submodule update --init --recursive
mise install && mise run install-dev-tools
make                    # build
make test               # run tests
mise run format-check   # lint
mise run format-fix     # auto-format
```

## Adding a New Table Function

1. Define bind data struct (or reuse `McapFileBindData`)
2. Define global state struct if needed
3. Implement `Bind`, `InitGlobal`, `Function` (scan) static functions
4. Register in `LoadInternal()` with `TableFunction` + `loader.RegisterFunction()`

## CDR Deserialization Flow

1. **Bind time**: Open MCAP, find channel by topic, parse schema with `cdr::MsgParser::Parse()`, generate DuckDB `LogicalType` via `FieldDefToLogical()`
2. **Scan time**: For each message, create `cdr::CdrReader` from raw bytes, call `CdrFieldToValue()` / `CdrArrayToValue()` per field, write to output columns

## CI

- Format check: `clang-format 11`, `black 24`, `cmake-format` (via `duckdb/scripts/format.py`)
- Tidy check: `clang-tidy` (via `duckdb/scripts/run-clang-tidy.py`)
- Build + test: `_extension_distribution.yml` (Linux amd64/arm64/musl, macOS, Windows, Wasm)
