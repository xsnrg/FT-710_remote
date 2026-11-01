cmake_minimum_required(VERSION 3.21)
project(ft710-remote VERSION 1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Xml DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
qt_standard_project_setup()
qt_add_executable(ft710-remote main.cpp netaudio.h txgate.h)
# Network-audio templates (sender side from server/netaudio, receiver side from app/netaudio),
# compiled in so the app can generate the configs it uploads / runs.
set(NETAUDIO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/netaudio/sender-roc.conf.in
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/netaudio/sender-rtp.conf.in
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/netaudio/sender-rtp-opus.sh.in
    ${CMAKE_CURRENT_SOURCE_DIR}/netaudio/receiver-roc.conf.in
    ${CMAKE_CURRENT_SOURCE_DIR}/netaudio/receiver-rtp.conf.in)
set(NETAUDIO_RES)
foreach(f ${NETAUDIO_FILES})
    get_filename_component(n ${f} NAME)
    configure_file(${f} ${CMAKE_CURRENT_BINARY_DIR}/netaudio/${n} COPYONLY)
    list(APPEND NETAUDIO_RES ${CMAKE_CURRENT_BINARY_DIR}/netaudio/${n})
endforeach()
qt_add_resources(ft710-remote netaudio PREFIX / BASE ${CMAKE_CURRENT_BINARY_DIR} FILES ${NETAUDIO_RES})
target_compile_options(ft710-remote PRIVATE -Wall -Wextra)
target_link_libraries(ft710-remote PRIVATE Qt6::Widgets Qt6::Network Qt6::Xml Qt6::DBus PkgConfig::XCB PkgConfig::OPUS)
configure_file(ft710-remote.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/ft710-remote.desktop @ONLY)
install(TARGETS ft710-remote RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ft710-remote.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
