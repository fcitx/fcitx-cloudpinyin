include_directories(
    ${PROJECT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${FCITX4_FCITX_INCLUDE_DIRS}
    ${FCITX4_FCITX_CONFIG_INCLUDE_DIRS}
    ${FCITX4_FCITX_UTILS_INCLUDE_DIRS}
    ${LIBINTL_INCLUDE_DIR}
)

link_directories(
    ${CURL_LIBRARY_DIRS}
)

set(FCITX_CLOUDPINYIN_SRCS
    cloudpinyin.c
    cloudpinyinconifg.c
    fetch.c
)

fcitx_add_addon(fcitx-cloudpinyin ${FCITX_CLOUDPINYIN_SRCS})
add_definitions( -DLOCALEDIR=\"${CMAKE_INSTALL_PREFIX}/share/locale\" -D_GNU_SOURCE )
target_link_libraries( fcitx-cloudpinyin
                       ${CURL_LIBRARIES}
                       ${LIBINTL_LIBRARIES}
                       )

fcitx_add_addon_conf_file(fcitx-cloudpinyin.conf)
fcitx_add_configdesc_file(fcitx-cloudpinyin.desc)
