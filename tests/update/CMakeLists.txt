add_executable(update-test main.cpp ../../src/updaterelease.cpp)
target_include_directories(update-test PRIVATE ../../src)
target_link_libraries(update-test PRIVATE ${LEMON_QT_LIBNAME}::Test)

add_test(NAME update COMMAND update-test -o update-results.txt,txt -o -,txt)
set_tests_properties(update PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} TIMEOUT 30)
