project(fcitx-cloudpinyin)
cmake_minimum_required(VERSION 2.6)

find_package(Fcitx REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Gettext REQUIRED)

# uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

SET(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules ${CMAKE_MODULE_PATH})

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

pkg_check_modules(CURL "libcurl" REQUIRED)
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--no-undefined,--as-needed")
set(CMAKE_MODULE_LINKER_FLAGS "-Wl,--no-undefined,--as-needed")

add_subdirectory(src)
add_subdirectory(po)