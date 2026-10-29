# hello-c — the smallest application for Hyggshi OS: a program, a Debian package, nothing else.
#
#   cmake -S . -B build && cmake --build build      the program in build/hello
#   cpack -G DEB --config build/CPackConfig.cmake    the package (or: debuild -us -uc)
cmake_minimum_required(VERSION 3.16)
project(hello-c VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# NexSDK: the system, notifications and settings of Hyggshi OS and HDE, without a toolkit.
find_package(PkgConfig REQUIRED)
pkg_check_modules(NEXSDK REQUIRED nexsdk)

add_executable(hello src/main.c)
target_include_directories(hello PRIVATE ${NEXSDK_INCLUDE_DIRS})
target_link_directories(hello PRIVATE ${NEXSDK_LIBRARY_DIRS})
target_link_libraries(hello PRIVATE ${NEXSDK_LIBRARIES})
target_compile_options(hello PRIVATE -Wall -Wextra -Wpedantic)

include(GNUInstallDirs)
install(TARGETS hello RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/hello.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

set(CPACK_PACKAGE_NAME "hello-c")
set(CPACK_PACKAGE_VENDOR "Hyggshi OS Research Technology")
set(CPACK_PACKAGE_CONTACT "you@example.com")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Your Name <you@example.com>")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "nexsdk (>= 1.0)")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
include(CPack)
