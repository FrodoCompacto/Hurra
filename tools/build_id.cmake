execute_process(COMMAND git describe --always --dirty --abbrev=12 --exclude=*
                RESULT_VARIABLE result OUTPUT_VARIABLE revision
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT result STREQUAL "0")
    set(revision "unknown")
endif()
set(content "#define HURRA_BUILD_ID \"${revision}\"\n")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" previous)
    if(content STREQUAL previous)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
