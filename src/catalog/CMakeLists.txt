# Library containing schema management: TableDef, TableDefBuilder, and the
# Catalog class that owns @meta/@table.
add_library(catalog
    value.cpp
    record.cpp
    tabledef.cpp
    catalog.cpp
)
target_include_directories(catalog PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(catalog PUBLIC storage)
