cmake_minimum_required(VERSION 3.20)
project(minisql VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(minisql_lib
  src/ast.cpp
  src/binder.cpp
  src/catalog.cpp
  src/csv.cpp
  src/database.cpp
  src/error.cpp
  src/executor.cpp
  src/format.cpp
  src/functions.cpp
  src/lexer.cpp
  src/operators.cpp
  src/parser.cpp
  src/planner.cpp
  src/shell.cpp
  src/value.cpp
)
set_target_properties(minisql_lib PROPERTIES OUTPUT_NAME minisql)
target_include_directories(minisql_lib PUBLIC include PRIVATE src)
target_compile_options(minisql_lib PRIVATE -Wall -Wextra)

add_executable(minisql src/main.cpp)
target_link_libraries(minisql PRIVATE minisql_lib)
target_compile_options(minisql PRIVATE -Wall -Wextra)

enable_testing()
add_subdirectory(tests)
