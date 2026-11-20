cmake_minimum_required(VERSION 3.22)

include("$ENV{IDF_PATH}/tools/cmake/project.cmake")
idf_build_set_property(MINIMAL_BUILD ON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" GADGET_METADATA)
string(JSON PROJECT_VER GET "${GADGET_METADATA}" version)
project(gadget)

# BadgeELF pulls in PAX codecs. Its zlib sources use pre-C99 definitions.
foreach(zlib_target zlib zlibstatic)
    if(TARGET ${zlib_target})
        target_compile_options(${zlib_target} PRIVATE -Wno-error=old-style-definition -Wno-error=implicit-fallthrough)
    endif()
endforeach()
