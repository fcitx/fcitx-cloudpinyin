include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${FCITX4_FCITX_INCLUDE_DIRS}
    ${FCITX4_FCITX_CONFIG_INCLUDE_DIRS}
    ${FCITX4_FCITX_UTILS_INCLUDE_DIRS}
    ${LIBINTL_INCLUDE_DIR}
    ${PTHREAD_INCLUDE_DIR}
)

link_directories(
    ${CURL_LIBRARY_DIRS}
    ${FCITX4_FCITX_UTILS_LIBRARY_DIRS}
)

add_executable(testhalf testhalf.c ../src/parse.c)

target_link_libraries( testhalf
                       ${PTHREAD_LIBRARIES}
                       ${FCITX4_FCITX_UTILS_LIBRARIES}
                       ${CURL_LIBRARIES}
                       ${LIBINTL_LIBRARIES}
                       )

add_test(testhalf testhalf)