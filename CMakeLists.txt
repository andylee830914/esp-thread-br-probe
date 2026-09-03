# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# This should be done before using the IDF_TARGET variable.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

project(thread_border_router)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)

# For RISCV chips, project_include.cmake sets -Wno-format, but does not clear various
# flags that depend on -Wformat
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wno-error=non-virtual-dtor" APPEND)
