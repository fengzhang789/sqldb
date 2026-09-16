# Library containing the query-language front-end: a flex scanner and a bison parser that turn SQL text into the
# QLNode AST in ql/ql_ast.h. Nothing outside this directory sees flex or bison; callers use parse_statement.

# macOS still ships bison 2.3, which predates the C++ variant API this grammar is written against, so look in the
# usual Homebrew prefixes before falling back to whatever is on PATH.
find_program(BISON_EXECUTABLE NAMES bison
    HINTS /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin
    DOC "GNU bison 3.3 or newer")
find_package(BISON 3.3 REQUIRED)
find_package(FLEX 2.6 REQUIRED)

if(BISON_VERSION VERSION_LESS 3.3)
    message(FATAL_ERROR "bison ${BISON_VERSION} is too old for src/ql/parser.y (needs 3.3+); try `brew install bison`")
endif()

bison_target(ql_parser parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cc
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.hh
    REPORT_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.output # the state machine, and any conflicts in it
    COMPILE_FLAGS "-Wall -v") # -v so the report is written at all
flex_target(ql_lexer lexer.l ${CMAKE_CURRENT_BINARY_DIR}/lexer.yy.cc
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/lexer.yy.h)
add_flex_bison_dependency(ql_lexer ql_parser)

add_library(ql
    ql_ast.cpp
    ql_parse.cpp
    ql_range.cpp
    ql_exec.cpp
    ${BISON_ql_parser_OUTPUTS}
    ${FLEX_ql_lexer_OUTPUTS}
)
target_include_directories(ql
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR} # the generated parser.tab.hh, location.hh and lexer.yy.h
)
target_link_libraries(ql PUBLIC db_api catalog storage)
