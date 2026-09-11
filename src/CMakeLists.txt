# Library containing the btree implementation, shared by the executable and tests
add_library(btree
    btree.cpp
)
target_include_directories(btree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Define the executable and specify its source files
add_executable(db
    main.cpp
)
target_link_libraries(db PRIVATE btree)
