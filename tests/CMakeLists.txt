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

include(GoogleTest)
gtest_discover_tests(btree_tests)
gtest_discover_tests(kv_tests)
gtest_discover_tests(pagemanager_tests)
gtest_discover_tests(freelist_tests)
