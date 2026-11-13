# Frametop app windows on Hyprland: the launcher of the desktop session's own Vibepollo, its
# user unit and its firewall profiles (docs/frametop-app-windows.md). Nothing here is enabled by the
# package: the desktop user turns it on with `vibepollo-frametop-windows setup`.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR SUNSHINE_BUILD_APPIMAGE OR
        SUNSHINE_BUILD_FLATPAK OR SUNSHINE_BUILD_STEAMOS)
    return()
endif()

get_filename_component(_frametop_windows_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
configure_file("${_frametop_windows_source_root}/packaging/linux/vibepollo-frametop-windows.service.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vibepollo-frametop-windows.service" @ONLY)
install(PROGRAMS "${_frametop_windows_source_root}/packaging/linux/vibepollo-frametop-windows"
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(SYSTEMD_FOUND)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibepollo-frametop-windows.service"
            DESTINATION "${SYSTEMD_USER_UNIT_INSTALL_DIR}")
endif()
install(FILES "${_frametop_windows_source_root}/packaging/linux/firewalld/vibepollo-frametop-windows.xml"
        DESTINATION "lib/firewalld/services")
install(FILES "${_frametop_windows_source_root}/packaging/linux/ufw/vibepollo-frametop-windows"
        DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/ufw/applications.d")
