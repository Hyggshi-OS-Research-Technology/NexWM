# gtk-app — a GTK3 application for Hyggshi OS, with the things a real application needs:
# a window, a menu entry, an icon, and NexSDK for the system it runs on.
#
#   cmake -S . -B build && cmake --build build
#   ./build/hello-gtk
#   hde-pkg-check ../*.deb          after debuild
cmake_minimum_required(VERSION 3.16)
project(gtk-app VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(NEXSDK REQUIRED nexsdk)

add_executable(hello-gtk src/main.c)
target_include_directories(hello-gtk PRIVATE ${GTK3_INCLUDE_DIRS} ${NEXSDK_INCLUDE_DIRS})
target_link_directories(hello-gtk PRIVATE ${NEXSDK_LIBRARY_DIRS})
target_link_libraries(hello-gtk PRIVATE ${GTK3_LIBRARIES} ${NEXSDK_LIBRARIES})
target_compile_options(hello-gtk PRIVATE -Wall -Wextra)

include(GNUInstallDirs)
install(TARGETS hello-gtk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/hello-gtk.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# the icon has to be in the package as well: a menu entry whose Icon= is nowhere is reported by hde-pkg-check
install(FILES data/hello-gtk.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

set(CPACK_PACKAGE_NAME "gtk-app")
set(CPACK_PACKAGE_VENDOR "Hyggshi OS Research Technology")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Your Name <you@example.com>")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-3-0, nexsdk (>= 1.0)")
include(CPack)
