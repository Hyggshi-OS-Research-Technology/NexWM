# library — a small C library packaged for Hyggshi OS: the header, the .so, the pkg-config file and a .deb.
cmake_minimum_required(VERSION 3.16)
project(mylib VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(mylib SHARED src/mylib.c)
target_include_directories(mylib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(mylib PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(mylib PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
install(TARGETS mylib LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/mylib.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# pkg-config: `pkg-config --cflags --libs mylib` in every program that uses the library
configure_file(mylib.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mylib.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mylib.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

set(CPACK_PACKAGE_NAME "libmylib")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Your Name <you@example.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6")
include(CPack)
