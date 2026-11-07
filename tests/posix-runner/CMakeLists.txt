add_executable(posix-runner-helper helper.cpp)
set_target_properties(posix-runner-helper PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_executable(posix-runner-tests main.cpp)
target_include_directories(posix-runner-tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${spdlog_DIR}/include)
target_link_libraries(posix-runner-tests PRIVATE lemon-core SingleApplication::SingleApplication
    ${LEMON_QT_LIBNAME}::Test ${LEMON_QT_LIBNAME}::Concurrent spdlog::spdlog)
add_dependencies(posix-runner-tests posix-runner-helper)
target_compile_definitions(posix-runner-tests PRIVATE HELPER_PATH="$<TARGET_FILE:posix-runner-helper>")
add_test(NAME posix-runner COMMAND posix-runner-tests)
set_tests_properties(posix-runner PROPERTIES TIMEOUT 20)
