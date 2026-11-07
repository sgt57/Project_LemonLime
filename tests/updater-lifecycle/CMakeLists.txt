add_executable(updater-lifecycle-test main.cpp)
target_link_libraries(updater-lifecycle-test PRIVATE ${LEMON_QT_LIBNAME}::Test)

foreach(version IN ITEMS old new)
    add_executable(updater-fixture-${version} fixture.cpp)
    target_compile_definitions(updater-fixture-${version} PRIVATE FIXTURE_VERSION="${version}")
    set_target_properties(updater-fixture-${version} PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    if(WIN32)
        target_link_libraries(updater-fixture-${version} PRIVATE shell32)
    endif()
    if(MSVC)
        set_property(TARGET updater-fixture-${version} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(WIN32)
        target_link_options(updater-fixture-${version} PRIVATE -static)
    endif()
endforeach()

add_dependencies(updater-lifecycle-test lemon-updater updater-fixture-old updater-fixture-new)
target_compile_definitions(updater-lifecycle-test PRIVATE
    UPDATER_PATH="$<TARGET_FILE:lemon-updater>"
    OLD_FIXTURE_PATH="$<TARGET_FILE:updater-fixture-old>"
    NEW_FIXTURE_PATH="$<TARGET_FILE:updater-fixture-new>"
    CMAKE_PATH="${CMAKE_COMMAND}")
add_test(NAME updater-lifecycle COMMAND updater-lifecycle-test -o -,txt)
set_tests_properties(updater-lifecycle PROPERTIES TIMEOUT 90)
