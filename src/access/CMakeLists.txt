# Library containing row-level encode/decode and point query/insert/update/delete.
add_library(access
    row_codec.cpp
    table_access.cpp
)
target_include_directories(access PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(access PUBLIC catalog storage)
