# Library containing the B+tree and KV store implementation, shared by the
# executable and tests.
find_package(Threads REQUIRED)

add_library(storage
    btree.cpp
    btree_iter.cpp
    freelist.cpp
    kv.cpp
    kv_reader.cpp
    reader_heap.cpp
)
target_include_directories(storage PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(storage PUBLIC Threads::Threads)
