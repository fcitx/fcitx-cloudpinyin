include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FCITX4_FCITX_INCLUDE_DIRS}
    ${FCITX4_FCITX_CONFIG_INCLUDE_DIRS}
    ${FCITX4_FCITX_UTILS_INCLUDE_DIRS}
)

link_directories(
    ${CURL_LIBRARY_DIRS}
    ${FCITX4_FCITX_LIBRARY_DIRS}
    ${FCITX4_FCITX_CONFIG_LIBRARY_DIRS}
    ${FCITX4_FCITX_UTILS_LIBRARY_DIRS}
)

set(FCITX_REMOTE_MODULE_SOURCES
    remote.c
)

set(FCITX_CLOUDPINYIN_SRCS
    cloudpinyin.c
)

fcitx_add_addon(fcitx-cloudpinyin ${FCITX_CLOUDPINYIN_SRCS})
target_link_libraries( fcitx-cloudpinyin fcitx-core fcitx-utils ${CURL_LIBRARIES})

fcitx_add_addon_conf_file(fcitx-cloudpinyin.conf)
