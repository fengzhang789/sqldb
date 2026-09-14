# Library containing the B+tree and KV store implementation, shared by the
# executable and tests.
add_library(storage
    btree.cpp
    btree_iter.cpp
    freelist.cpp
    kv.cpp
    pagemanager.cpp
)
target_include_directories(storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
