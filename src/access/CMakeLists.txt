# Library containing row-level encode/decode, point query/insert/update/delete, and range scans.
add_library(access
    row_codec.cpp
    scanner.cpp
    table_access.cpp
)
target_include_directories(access PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(access PUBLIC catalog encoding storage)
