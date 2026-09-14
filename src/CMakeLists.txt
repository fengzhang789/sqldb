add_subdirectory(storage)
add_subdirectory(catalog)
add_subdirectory(encoding)
add_subdirectory(access)

# Define the executable and specify its source files
add_executable(db
    main.cpp
    db.cpp
)
target_include_directories(db PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(db PRIVATE storage catalog encoding access)
