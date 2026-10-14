configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

qt_add_executable(starfish WIN32
    main.cpp
    # Application manifest. Load bearing: see src/starfish.manifest (ADR-4).
    starfish.rc
    core/desktop_item.cpp
    core/rules.cpp
    core/jsonstore.cpp
    core/layout_store.cpp
    core/zorder.cpp
    platform/win/desktop_host.cpp
    platform/win/desktop_surface.cpp
    platform/win/desktop_scanner.cpp
    platform/win/shell_icon_cache.cpp
    platform/win/shell_context_menu.cpp
    platform/win/shell_drop_target.cpp
    ui/fence_container.cpp
    ui/fence_view.cpp
    ui/icon_grid.cpp
)

target_include_directories(starfish PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(starfish PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    # Win32 desktop integration:
    #   user32   Progman / SHELLDLL_DefView, layered windows (ADR-4)
    #   shell32  SHGetKnownFolderPath, SHGetFileInfoW, SHGetImageList (T-004)
    #   ole32    CoInitializeEx, CoCreateInstance, CoTaskMemFree (T-004)
    #   uuid     CLSID_ShellLink / IID_IShellLinkW / IID_IImageList symbols
    #   dwmapi   DwmGetWindowAttribute(DWMWA_CLOAKED) diagnostic (T-020)
    user32
    shell32
    ole32
    uuid
    dwmapi
)
