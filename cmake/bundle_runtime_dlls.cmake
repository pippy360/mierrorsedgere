# Copies the DLLs an executable needs at run time next to it (cmake -P script).
#   ME_EXECUTABLE   the executable
#   ME_SEARCH_DIRS  where its DLLs are found (the toolchain's bin directory)
#   ME_OBJDUMP      objdump, to read import tables
# Windows' own DLLs are left alone. Files already up to date are not copied again.

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)  # paths are normalized before the regexes below see them
endif()

if(NOT ME_EXECUTABLE OR NOT EXISTS "${ME_EXECUTABLE}")
    message(FATAL_ERROR "bundle_runtime_dlls: ME_EXECUTABLE is not set or missing")
endif()

set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
if(ME_OBJDUMP)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${ME_OBJDUMP}")
endif()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${ME_EXECUTABLE}"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    DIRECTORIES ${ME_SEARCH_DIRS}
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]"
)

get_filename_component(out_dir "${ME_EXECUTABLE}" DIRECTORY)
set(copied 0)
foreach(dll IN LISTS resolved)
    get_filename_component(dll_dir "${dll}" DIRECTORY)
    if(dll_dir STREQUAL out_dir)
        continue()
    endif()
    file(COPY "${dll}" DESTINATION "${out_dir}")
    math(EXPR copied "${copied} + 1")
endforeach()
message(STATUS "Runtime DLLs copied next to the executable: ${copied}")
