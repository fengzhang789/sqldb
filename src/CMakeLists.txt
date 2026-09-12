add_subdirectory(storage)

# Define the executable and specify its source files
add_executable(db
    main.cpp
)
target_link_libraries(db PRIVATE storage)
