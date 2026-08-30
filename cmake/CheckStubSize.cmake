# Post-build guard for the stub.
#
# Two things are enforced, both of which are easy to regress silently:
#   1. No source under src/stub/ includes a heavyweight formatting header.
#      Adding one costs roughly 290 KB in every packaged application.
#   2. The linked binary stays under a size ceiling.
#
# Both are warnings rather than hard errors, so a deliberate change is not
# blocked, but an accidental one is impossible to miss.

set(SIZE_WARN_KB 420)

file(GLOB_RECURSE _stub_sources "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.hpp")
set(_banned "<print>" "<format>" "<iostream>" "<sstream>" "<ostream>" "<istream>")

foreach(_src ${_stub_sources})
    file(READ "${_src}" _text)
    foreach(_hdr ${_banned})
        string(REPLACE "<" "" _bare "${_hdr}")
        string(REPLACE ">" "" _bare "${_bare}")
        if(_text MATCHES "#[ \t]*include[ \t]*<${_bare}>")
            get_filename_component(_name "${_src}" NAME)
            message(WARNING
                "hnxstub: ${_name} includes ${_hdr}. This pulls the C++ formatting "
                "machinery into every packaged application (~290 KB measured). "
                "Use Win32 (wsprintfW / MessageBoxW / WriteConsoleW) instead.")
        endif()
    endforeach()
endforeach()

if(EXISTS "${STUB_EXE}")
    file(SIZE "${STUB_EXE}" _bytes)
    math(EXPR _kb "${_bytes} / 1024")
    if(_kb GREATER SIZE_WARN_KB)
        message(WARNING
            "hnxstub is ${_kb} KB, above the ${SIZE_WARN_KB} KB budget. "
            "Check for a newly included heavyweight header or a codec linked by mistake.")
    else()
        message(STATUS "hnxstub size: ${_kb} KB (budget ${SIZE_WARN_KB} KB)")
    endif()
endif()
