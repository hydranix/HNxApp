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
    test_walk.cpp
    test_meta.cpp

    ${PROJECT_SOURCE_DIR}/src/stub/meta.cpp
    ${PROJECT_SOURCE_DIR}/src/stub/winpe.cpp
    ${PROJECT_SOURCE_DIR}/src/stub/toc.cpp
    ${PROJECT_SOURCE_DIR}/src/stub/selfmap.cpp
    ${PROJECT_SOURCE_DIR}/src/stub/codec/codec.cpp
    ${PROJECT_SOURCE_DIR}/src/packer/appid.cpp
    ${PROJECT_SOURCE_DIR}/src/packer/cmdline.cpp
    ${PROJECT_SOURCE_DIR}/src/packer/walk.cpp
    ${PROJECT_SOURCE_DIR}/src/packer/console.cpp
)

target_link_libraries(hnx_tests PRIVATE hnx_common zstd_full lz4 xxhash hnx_warnings)
target_link_libraries(hnx_tests PRIVATE kernel32 user32 shell32 ole32 advapi32 version)
target_include_directories(hnx_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/packer
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/stub
    ${PROJECT_SOURCE_DIR}/src/common)

add_test(NAME hnx_tests COMMAND hnx_tests)
