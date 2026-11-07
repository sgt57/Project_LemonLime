# Native updater: no Qt dependency or runtime files from the installation directory.
find_package(Threads REQUIRED)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h" "#pragma once\n#define MINIZ_EXPORT\n")
add_library(lemon-miniz STATIC
    ${CMAKE_SOURCE_DIR}/3rdparty/miniz/miniz.c
    ${CMAKE_SOURCE_DIR}/3rdparty/miniz/miniz_tinfl.c
    ${CMAKE_SOURCE_DIR}/3rdparty/miniz/miniz_zip.c
    )
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/3rdparty/miniz/miniz.c
    ${CMAKE_SOURCE_DIR}/3rdparty/miniz/miniz_tinfl.c
    ${CMAKE_SOURCE_DIR}/3rdparty/miniz/miniz_zip.c
    PROPERTIES LANGUAGE CXX)
target_compile_features(lemon-miniz PRIVATE cxx_std_23)
target_include_directories(lemon-miniz SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/3rdparty/miniz ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(lemon-miniz PUBLIC
    MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_DEFLATE_APIS
    MINIZ_NO_ZLIB_APIS MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
aux_source_directory(${CMAKE_SOURCE_DIR}/src/updater LEMON_UPDATER_SOURCES)

if(WIN32)
    add_executable(lemon-updater WIN32 ${LEMON_UPDATER_SOURCES})
    target_link_libraries(lemon-updater PRIVATE shell32 user32)
else()
    add_executable(lemon-updater ${LEMON_UPDATER_SOURCES})
endif()

target_link_libraries(lemon-updater PRIVATE lemon-miniz Threads::Threads)
if(APPLE)
    target_link_libraries(lemon-updater PRIVATE "-framework CoreFoundation")
endif()

set_target_properties(lemon-updater PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native-updater/$<CONFIG>"
    )
if(MSVC)
    set_property(TARGET lemon-updater lemon-miniz PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(WIN32)
    target_link_options(lemon-updater PRIVATE -static -municode)
endif()
set(LEMON_UPDATER_QRC "${CMAKE_CURRENT_BINARY_DIR}/lemon-updater.qrc")
add_custom_command(
    OUTPUT "${LEMON_UPDATER_QRC}"
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${LEMON_UPDATER_QRC}
        -DINPUT=$<TARGET_FILE:lemon-updater>
        -P "${CMAKE_SOURCE_DIR}/cmake/embed-updater-qrc.cmake"
    DEPENDS lemon-updater
    VERBATIM
    )
set_source_files_properties("${LEMON_UPDATER_QRC}" PROPERTIES GENERATED TRUE)

