# Library containing the order-preserving encoding of typed values, used for table keys and rows.
add_library(encoding
    order_preserving.cpp
)
target_include_directories(encoding PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(encoding PUBLIC catalog)
