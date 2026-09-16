add_subdirectory(storage)
add_subdirectory(catalog)
add_subdirectory(encoding)
add_subdirectory(access)

# Library containing the top-level DB API and its transactions, shared by the executable and tests.
add_library(db_api
    db.cpp
)
target_include_directories(db_api PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(db_api PUBLIC access catalog storage)

# The query language: parser front-end plus the executor, which runs statements through db_api.
add_subdirectory(ql)

# Define the executable and specify its source files
add_executable(db
    main.cpp
)
target_include_directories(db PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(db PRIVATE db_api)
