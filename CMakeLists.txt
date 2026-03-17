cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME mcap)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)
include_directories(third_party/mcap/include)
include_directories(third_party/lz4)

set(EXTENSION_SOURCES
    src/mcap_extension.cpp
    third_party/lz4/lz4.c
    third_party/lz4/lz4hc.c
    third_party/lz4/lz4frame.c
    third_party/lz4/xxhash.c)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# MCAP headers require C++17
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)

# zstd: use DuckDB's bundled zstd (duckdb_zstd namespace, resolved via using-directive in source)
target_include_directories(${EXTENSION_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/../duckdb/third_party/zstd/include)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/../duckdb/third_party/zstd/include)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
