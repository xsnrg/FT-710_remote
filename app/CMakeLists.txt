cmake_minimum_required(VERSION 3.21)
project(ft710-remote VERSION 1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
qt_standard_project_setup()
qt_add_executable(ft710-remote main.cpp netaudio.h txgate.h tune.h devname.h)
# Network-audio templates (sender side from server/netaudio, receiver side from app/netaudio),
# compiled in so the app can generate the configs it uploads / runs.
set(NETAUDIO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/netaudio/sender-roc.conf.in
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/netaudio/sender-rtp.conf.in
    ${CMAKE_CURRENT_SOURCE_DIR}/../server/netaudio/sender-rtp-opus.sh.in
    ${CMAKE_CURRENT_SOURCE_DIR}/netaudio/receiver-roc.conf.in
    ${CMAKE_CURRENT_SOURCE_DIR}/netaudio/receiver-rtp.conf.in
    ${CMAKE_CURRENT_SOURCE_DIR}/netaudio/receiver-rxgain.inc.in)
set(NETAUDIO_RES)
foreach(f ${NETAUDIO_FILES})
    get_filename_component(n ${f} NAME)
    configure_file(${f} ${CMAKE_CURRENT_BINARY_DIR}/netaudio/${n} COPYONLY)
    list(APPEND NETAUDIO_RES ${CMAKE_CURRENT_BINARY_DIR}/netaudio/${n})
endforeach()
# RX gain + lookahead limiter (LADSPA) for the network-audio receiver, compiled in as a resource too
# (the app writes it out at receiver start, so the binary stays self-contained).
set(RXLIMIT_SO ${CMAKE_CURRENT_BINARY_DIR}/netaudio/ft710_rxlimit.so)
add_custom_command(OUTPUT ${RXLIMIT_SO}
    COMMAND ${CMAKE_C_COMPILER} -O2 -Wall -Wextra -shared -fPIC -o ${RXLIMIT_SO} ${CMAKE_CURRENT_SOURCE_DIR}/netaudio/ft710_rxlimit.c -lm
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/netaudio/ft710_rxlimit.c
    COMMENT "Building the RX gain/limiter LADSPA plugin")
list(APPEND NETAUDIO_RES ${RXLIMIT_SO})
qt_add_resources(ft710-remote netaudio PREFIX / BASE ${CMAKE_CURRENT_BINARY_DIR} FILES ${NETAUDIO_RES})
target_compile_options(ft710-remote PRIVATE -Wall -Wextra)
target_link_libraries(ft710-remote PRIVATE Qt6::Widgets Qt6::Network PkgConfig::OPUS)
configure_file(ft710-remote.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/ft710-remote.desktop @ONLY)
install(TARGETS ft710-remote RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ft710-remote.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# Tests: ctest (or the scripts directly). test_ui_modes.py drives the built app offscreen.
enable_testing()
add_executable(test_tune_round tests/test_tune_round.cpp)
add_test(NAME tune_round COMMAND test_tune_round)
add_executable(test_step_round tests/test_step_round.cpp)
add_test(NAME step_round COMMAND test_step_round)
add_executable(test_devname tests/test_devname.cpp)
target_link_libraries(test_devname PRIVATE Qt6::Core)
add_test(NAME devname COMMAND test_devname)
add_executable(test_rxlimit tests/test_rxlimit.c)
target_link_libraries(test_rxlimit PRIVATE ${CMAKE_DL_LIBS} m)
add_custom_target(rxlimit_so ALL DEPENDS ${RXLIMIT_SO})
add_test(NAME rxlimit COMMAND test_rxlimit ${RXLIMIT_SO})
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME ui_modes COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ui_modes.py $<TARGET_FILE:ft710-remote>)
    add_test(NAME txgate_catreset COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../server/ft710-txgate/test_catreset.py)
endif()
# hover preview == stripe after the click, preview centred on the cursor, axis uses the radio's scope centre
add_test(NAME scope_preview COMMAND $<TARGET_FILE:ft710-remote> --scope-selftest)
set_tests_properties(scope_preview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;DISPLAY=;WAYLAND_DISPLAY=")
# arrow-key routing: key map, keypad modifier, focus rules (static logic, offscreen)
add_test(NAME tune_keys COMMAND $<TARGET_FILE:ft710-remote> --tune-keys-selftest)
set_tests_properties(tune_keys PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;DISPLAY=;WAYLAND_DISPLAY=")
