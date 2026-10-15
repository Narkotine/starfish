configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

qt_add_executable(starfish WIN32
    main.cpp
    # Application manifest. Load bearing: see src/starfish.manifest (ADR-4).
    starfish.rc
    app/product_app.cpp
    core/desktop_item.cpp
    core/desktop_watcher.cpp
    core/icon_hiding_marker.cpp
    core/portal.cpp
    core/rules.cpp
    core/jsonstore.cpp
    core/layout_store.cpp
    core/snapshot.cpp
    core/zorder.cpp
    platform/win/desktop_host.cpp
    platform/win/desktop_surface.cpp
    platform/win/desktop_scanner.cpp
    platform/win/shell_icon_cache.cpp
    platform/win/shell_context_menu.cpp
    platform/win/shell_drop_target.cpp
    platform/win/hotkey_manager.cpp
    ui/fence_container.cpp
    ui/settings_dialog.cpp
    ui/tray_icon.cpp
    ui/fence_view.cpp
    ui/icon_grid.cpp
)

target_include_directories(starfish PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# MSVC embeds a manifest of its own (/MANIFEST:EMBED,ID=1) while starfish.rc
# already embeds ours under resource id 1 -- two RT_MANIFEST resources with the
# same id, which CVTRES refuses ("CVT1100: duplicate resource") and the link then
# dies with LNK1123. MinGW has no such default step, which is why this was
# invisible until the first MSVC build on CI (2026-10-15). The manifest has to
# stay embedded by hand on both toolchains: it is what makes WS_EX_LAYERED
# work on a child window (ADR-4), and its absence is silent.
if(MSVC)
    target_link_options(starfish PRIVATE "/MANIFEST:NO")
endif()

# Deployment is load bearing too, for the same reason as the manifest: the MinGW
# build links Qt dynamically, so `build\starfish.exe` refuses to start with
# "Qt6Widgets.dll introuvable" unless Qt's bin directory happens to be on PATH.
# Nobody launching the demo from a plain shell has that. windeployqt copies the
# Qt DLLs, the platform plugin and the MinGW runtime next to the executable, so
# the binary is self-contained and double-clickable.
# POST_BUILD only fires when the target relinks, so this costs nothing on
# incremental builds. Skipped silently if windeployqt cannot be located.
if(WIN32)
    get_target_property(_starfish_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_starfish_qt_bin_dir "${_starfish_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_starfish_qt_bin_dir}")
    if(WINDEPLOYQT_EXE)
        add_custom_command(TARGET starfish POST_BUILD
            COMMAND "${WINDEPLOYQT_EXE}" --release
                    --no-translations --no-system-d3d-compiler --no-opengl-sw
                    --compiler-runtime
                    "$<TARGET_FILE:starfish>"
            COMMENT "Deploying the Qt runtime next to starfish.exe"
            VERBATIM)
    else()
        message(STATUS "windeployqt not found: build/starfish.exe will need Qt's bin on PATH")
    endif()
endif()

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
