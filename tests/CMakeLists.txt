include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(btree_tests
    btree_test.cpp
)
target_link_libraries(btree_tests PRIVATE storage GTest::gtest_main)

add_executable(btree_iter_tests
    btree_iter_test.cpp
)
target_link_libraries(btree_iter_tests PRIVATE storage GTest::gtest_main)

add_executable(kv_tests
    kv_test.cpp
)
target_link_libraries(kv_tests PRIVATE storage GTest::gtest_main)

add_executable(pagemanager_tests
    pagemanager_test.cpp
)
target_link_libraries(pagemanager_tests PRIVATE storage GTest::gtest_main)

add_executable(freelist_tests
    freelist_test.cpp
)
target_link_libraries(freelist_tests PRIVATE storage GTest::gtest_main)

add_executable(reader_heap_tests
    reader_heap_test.cpp
)
target_link_libraries(reader_heap_tests PRIVATE storage GTest::gtest_main)

add_executable(catalog_tests
    catalog_test.cpp
)
target_link_libraries(catalog_tests PRIVATE catalog storage GTest::gtest_main)

add_executable(order_preserving_tests
    order_preserving_test.cpp
)
target_link_libraries(order_preserving_tests PRIVATE encoding catalog GTest::gtest_main)

add_executable(row_codec_tests
    row_codec_test.cpp
)
target_link_libraries(row_codec_tests PRIVATE access encoding catalog storage GTest::gtest_main)

add_executable(table_access_tests
    table_access_test.cpp
)
target_link_libraries(table_access_tests PRIVATE access catalog storage GTest::gtest_main)

add_executable(scanner_tests
    scanner_test.cpp
)
target_link_libraries(scanner_tests PRIVATE access encoding catalog storage GTest::gtest_main)

add_executable(index_tests
    index_test.cpp
)
target_link_libraries(index_tests PRIVATE access encoding catalog storage GTest::gtest_main)

add_executable(db_tests
    db_test.cpp
)
target_link_libraries(db_tests PRIVATE db_api GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(db_tests)
gtest_discover_tests(btree_tests)
gtest_discover_tests(btree_iter_tests)
gtest_discover_tests(order_preserving_tests)
gtest_discover_tests(scanner_tests)
gtest_discover_tests(index_tests)
gtest_discover_tests(kv_tests)
gtest_discover_tests(pagemanager_tests)
gtest_discover_tests(freelist_tests)
gtest_discover_tests(reader_heap_tests)
gtest_discover_tests(catalog_tests)
gtest_discover_tests(row_codec_tests)
gtest_discover_tests(table_access_tests)
