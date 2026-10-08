# Equivalent to Rabbitizer 1.7.10 tables/tools/c_table_gen.sh, using CMake
# instead of Bash/sed/cpp so native Windows builds need no shell tools.
# Upstream templates and resulting tables retain Rabbitizer's MIT license.
if(MSVC_MODE)
  set(_args /EP /TC "/I${TABLES}" "${INPUT}")
else()
  set(_args -E -P -x c -I "${TABLES}" "${INPUT}")
endif()
execute_process(COMMAND "${COMPILER}" ${_args}
  RESULT_VARIABLE _result OUTPUT_VARIABLE _table ERROR_VARIABLE _error)
if(NOT _result STREQUAL "0")
  message(FATAL_ERROR "Rabbitizer table generation failed for ${INPUT}: ${_error}")
endif()
get_filename_component(_name "${OUTPUT}" NAME)
string(REPLACE "." "_" _guard "${_name}_automatic")
get_filename_component(_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_directory}")
string(CONCAT _content
  "/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */\n"
  "/* SPDX-License-Identifier: MIT */\n"
  "/* Automatically generated. DO NOT MODIFY */\n"
  "#ifndef ${_guard}\n#define ${_guard}\n${_table}\n#endif\n")

if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" _existing)
  if(_existing STREQUAL _content)
    return()
  endif()
endif()
file(WRITE "${OUTPUT}" "${_content}")
