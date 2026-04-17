# Fails the build if the generated app binary exceeds MAX_BYTES.
# Invoked as: cmake -DBIN_FILE=... -DMAX_BYTES=... -P assert_bin_size.cmake
if(NOT BIN_FILE OR NOT MAX_BYTES)
    message(FATAL_ERROR "assert_bin_size: BIN_FILE and MAX_BYTES are required")
endif()
if(NOT EXISTS "${BIN_FILE}")
    # Binary not generated yet — not a post-build regression, skip.
    return()
endif()
file(SIZE "${BIN_FILE}" _bin_size)
if(_bin_size GREATER ${MAX_BYTES})
    message(FATAL_ERROR
        "cyberdeck.bin size ${_bin_size} > budget ${MAX_BYTES}. "
        "Investigate: run `idf.py size-components` and shed weight before shipping.")
endif()
math(EXPR _bin_kb "${_bin_size} / 1024")
math(EXPR _max_kb "${MAX_BYTES} / 1024")
message(STATUS "cyberdeck.bin size guard: ${_bin_size} bytes (~${_bin_kb} KB) <= ${MAX_BYTES} (~${_max_kb} KB) OK")
