# Tests link the stub and packer sources directly rather than driving the
# executables, so units are exercised in-process where failures point at a line
# rather than at an exit code.

add_executable(hnx_tests
    test_main.cpp
    test_paths.cpp
    test_format.cpp
    test_toc.cpp
    test_footer.cpp
    test_interactive.cpp
    test_cmdline.cpp
    test_winpe.cpp

    ${CMAKE_SOURCE_DIR}/src/stub/meta.cpp
    ${CMAKE_SOURCE_DIR}/src/stub/winpe.cpp
    ${CMAKE_SOURCE_DIR}/src/stub/toc.cpp
    ${CMAKE_SOURCE_DIR}/src/stub/selfmap.cpp
    ${CMAKE_SOURCE_DIR}/src/stub/codec/codec.cpp
    ${CMAKE_SOURCE_DIR}/src/packer/appid.cpp
    ${CMAKE_SOURCE_DIR}/src/packer/cmdline.cpp
    ${CMAKE_SOURCE_DIR}/src/packer/walk.cpp
    ${CMAKE_SOURCE_DIR}/src/packer/console.cpp
)

target_link_libraries(hnx_tests PRIVATE hnx_common zstd_full lz4 xxhash hnx_warnings)
target_link_libraries(hnx_tests PRIVATE kernel32 user32 shell32 ole32 advapi32 version)
target_include_directories(hnx_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/packer
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/stub
    ${CMAKE_SOURCE_DIR}/src/common)

add_test(NAME hnx_tests COMMAND hnx_tests)
