# Version from the nearest v* tag, git-describe style: "1.3.2" at a tag,
# "1.3.2-5-gb0d46a83" past it, "2.0.0-rc1" for a pre-release tag, "-dirty" appended
# when the tree has uncommitted changes. Everything before the first '-' must
# stay a dotted number: the Windows RC version is derived from it.
function(skate3_compute_version out_var)
    set(one_value FLOOR_VERSION GIT_DESCRIBE)
    cmake_parse_arguments(ARG "" "${one_value}" "" ${ARGN})

    if("${ARG_GIT_DESCRIBE}" STREQUAL "")
        message(WARNING
            "skate3_compute_version: no v* tag reachable from HEAD. "
            "Falling back to ${ARG_FLOOR_VERSION}-dev.unknown. "
            "For CI release builds, fetch tags and build from a release tag.")
        set(${out_var} "${ARG_FLOOR_VERSION}-dev.unknown" PARENT_SCOPE)
        return()
    endif()

    # Pre-release suffixes must start with a letter, so "-5-gabc1234" can only
    # ever parse as git describe's commit count, never as a pre-release.
    if(NOT ARG_GIT_DESCRIBE MATCHES "^v([0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?)(-[A-Za-z][0-9A-Za-z.]*)?(-[0-9]+-g[0-9a-f]+)?(-dirty)?$")
        # Warn rather than fail: a tag nobody can parse must not be able to stop
        # every build, which is what the no-tag path already does.
        message(WARNING
            "skate3_compute_version: unparseable describe output '${ARG_GIT_DESCRIBE}'. "
            "Falling back to ${ARG_FLOOR_VERSION}-dev.unknown.")
        set(${out_var} "${ARG_FLOOR_VERSION}-dev.unknown" PARENT_SCOPE)
        return()
    endif()
    set(${out_var} "${CMAKE_MATCH_1}${CMAKE_MATCH_4}${CMAKE_MATCH_5}${CMAKE_MATCH_6}" PARENT_SCOPE)
endfunction()

function(skate3_resolve_version out_var)
    set(one_value FLOOR_VERSION SOURCE_DIR)
    cmake_parse_arguments(ARG "" "${one_value}" "" ${ARGN})

    if(NOT ARG_SOURCE_DIR)
        set(ARG_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
    endif()

    set(describe "")
    find_program(GIT_EXECUTABLE git)
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --match "v[0-9]*.[0-9]*"
            WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
            OUTPUT_VARIABLE describe
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE describe_rc)
        if(NOT describe_rc EQUAL 0)
            set(describe "")
        endif()
    endif()

    skate3_compute_version(result
        FLOOR_VERSION ${ARG_FLOOR_VERSION}
        GIT_DESCRIBE "${describe}")
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()
