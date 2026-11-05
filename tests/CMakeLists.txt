set(MINISQL_TESTS
  test_lexer
  test_parser
  test_value
  test_select
  test_dml
  test_transaction
  test_index
  test_join
  test_aggregate
  test_explain
  test_errors
  test_csv
  test_format
  test_shell
)

foreach(name ${MINISQL_TESTS})
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE minisql_lib)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# The installed program, driven from the command line.
set(EXAMPLES ${PROJECT_SOURCE_DIR}/examples)

add_test(NAME cli_inline_sql COMMAND minisql -c "SELECT 1 + 2 AS three;")
set_tests_properties(cli_inline_sql PROPERTIES PASS_REGULAR_EXPRESSION "\\| +3 \\|")

add_test(NAME cli_help COMMAND minisql --help)
set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "usage: minisql")

add_test(NAME cli_script COMMAND minisql ${EXAMPLES}/library.sql)
set_tests_properties(cli_script PROPERTIES PASS_REGULAR_EXPRESSION "Dune")

add_test(NAME cli_syntax_error_fails COMMAND minisql -c "SELEC 1;")
set_tests_properties(cli_syntax_error_fails PROPERTIES WILL_FAIL TRUE)

add_test(NAME cli_error_names_position COMMAND minisql -c "SELECT * FROM nowhere;")
set_tests_properties(cli_error_names_position PROPERTIES PASS_REGULAR_EXPRESSION "line 1, column 15")

add_test(NAME cli_missing_file_fails COMMAND minisql ${EXAMPLES}/does-not-exist.sql)
set_tests_properties(cli_missing_file_fails PROPERTIES WILL_FAIL TRUE)

add_test(NAME cli_unknown_option_fails COMMAND minisql --nonsense)
set_tests_properties(cli_unknown_option_fails PROPERTIES WILL_FAIL TRUE)
