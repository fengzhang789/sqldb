# Library containing schema management: TableDef, TableDefBuilder, and the
# Catalog class that owns @meta/@table.
add_library(catalog
    value.cpp
    record.cpp
    tabledef.cpp
    catalog.cpp
)
target_include_directories(catalog PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(catalog PUBLIC storage)
