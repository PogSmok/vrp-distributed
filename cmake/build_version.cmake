# Writes OUTPUT with the git version of SOURCE_DIR, or FALLBACK outside git
# (ADR 0019). Run at every build; an unchanged file is not touched, so that
# nothing recompiles.
execute_process(
    COMMAND git describe --always --dirty --abbrev=12
    WORKING_DIRECTORY ${SOURCE_DIR}
    OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    set(version ${FALLBACK})
endif()
file(CONFIGURE OUTPUT ${OUTPUT} CONTENT
"#ifndef VRP_BUILD_VERSION_HPP_
#define VRP_BUILD_VERSION_HPP_

#define VRP_BUILD_VERSION \"@version@\"

#endif  // VRP_BUILD_VERSION_HPP_
" @ONLY)
