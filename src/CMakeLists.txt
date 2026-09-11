# Define the executable and specify its source files
add_executable(db
    main.cpp
    btree.cpp
)

# Set include directories relative to src
target_include_directories(db PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
