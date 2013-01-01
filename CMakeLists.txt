project(fcitx-cloudpinyin)
cmake_minimum_required(VERSION 2.6)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
find_package(Fcitx 4.2.7 REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Libintl REQUIRED)
find_package(Libiconv REQUIRED)
find_package(Pthread REQUIRED)

_fcitx_add_uninstall_target()

set(CMAKE_C_FLAGS "-Wall -Wextra -Wno-sign-compare -Wno-unused-parameter -fvisibility=hidden ${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wno-sign-compare -Wno-unused-parameter -fvisibility=hidden ${CMAKE_CXX_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--as-needed ${CMAKE_SHARED_LINKER_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS "-Wl,--as-needed ${CMAKE_MODULE_LINKER_FLAGS}")

pkg_check_modules(CURL "libcurl" REQUIRED)

set(LOCALEDIR "${CMAKE_INSTALL_PREFIX}/share/locale")

configure_file(config.h.in config.h)

enable_testing()

add_subdirectory(po)
add_subdirectory(src)
add_subdirectory(test)
