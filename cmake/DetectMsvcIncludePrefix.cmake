# SPDX-FileCopyrightText: 2026 Crow Translate contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Preserve the compiler's actual output bytes: CMake's automatic detection can
# transcode Japanese /showIncludes output, so Ninja silently misses every header
# dependency. English resources are not present in every MSVC installation.
set(_crow_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/crow-include-probe")
file(MAKE_DIRECTORY "${_crow_probe_dir}")
file(WRITE "${_crow_probe_dir}/crow-include-probe.h" "#pragma once\n")
file(WRITE "${_crow_probe_dir}/probe.cpp" "#include \"${_crow_probe_dir}/crow-include-probe.h\"\n")
execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c
        "${_crow_probe_dir}/probe.cpp" "/Fo${_crow_probe_dir}/probe.obj"
    OUTPUT_VARIABLE _crow_probe_output
    ERROR_VARIABLE _crow_probe_error
    RESULT_VARIABLE _crow_probe_result
    ENCODING NONE
)
if(_crow_probe_result EQUAL 0)
    string(REGEX MATCH "(^|\n)([^\r\n]+) ([A-Za-z]:[/\\\\]|\\\\\\\\)[^\r\n]*crow-include-probe.h"
        _crow_probe_match "${_crow_probe_output}")
    if(_crow_probe_match)
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2} ")
    endif()
endif()
unset(_crow_probe_dir)
unset(_crow_probe_output)
unset(_crow_probe_error)
unset(_crow_probe_result)
unset(_crow_probe_match)
