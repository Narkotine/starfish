configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

qt_add_executable(starfish WIN32
    main.cpp
    # Application manifest. Load bearing: see src/starfish.manifest (ADR-4).
    starfish.rc
    app/product_app.cpp
    app/drawn_desktop_probe.cpp
    app/context_menu_probe.cpp
    app/ui_probe.cpp
    core/desktop_item.cpp
    core/desktop_watcher.cpp
    core/icon_hiding_marker.cpp
    core/i18n.cpp
    core/portal.cpp
    core/rules.cpp
    core/jsonstore.cpp
    core/context_menu.cpp
    core/layout_store.cpp
    core/shortcut_vault.cpp
    core/snapshot.cpp
    core/zorder.cpp
    platform/win/desktop_host.cpp
    platform/win/desktop_surface.cpp
    platform/win/startup_timeline.cpp
    platform/win/desktop_icons.cpp
    platform/win/desktop_menu.cpp
    platform/win/desktop_shell_view.cpp
    platform/win/desktop_scanner.cpp
    platform/win/shell_icon_cache.cpp
    platform/win/shell_context_menu.cpp
    platform/win/shell_drop_target.cpp
    platform/win/shell_operations.cpp
    platform/win/autostart.cpp
    platform/win/windows_theme.cpp
    platform/win/packaged_commands.cpp
    platform/win/hotkey_manager.cpp
    ui/fence_container.cpp
    ui/modern_menu.cpp
    ui/context_menu_editor.cpp
    ui/toggle_switch.cpp
    ui/desktop_backdrop.cpp
    ui/settings_dialog.cpp
    ui/splash_screen.cpp
    ui/drag_image.cpp
    ui/tray_icon.cpp
    ui/fence_view.cpp
    ui/icon_grid.cpp
    ui/fence_widgets.cpp
    ui/note_editor.cpp
)

# StarFish's icon for the tray and the windows (session 59): the PNGs written by
# tools/make_icon.py next to the SVG they come from.
qt_add_resources(starfish "starfish_icons"
    PREFIX "/icons"
    BASE resources
    FILES
        resources/starfish-16.png
        resources/starfish-20.png
        resources/starfish-24.png
        resources/starfish-32.png
        resources/starfish-48.png
        resources/starfish-64.png
        resources/starfish-128.png
        resources/starfish-256.png
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

# A resource is not a tracked build input. Ninja knows starfish.rc itself, and
# nothing else: windres is not asked for a dependency file (see the RC rule in
# build.ninja -- it declares `depfile`, but the command line carries no -MD), so
# editing the manifest or the icon leaves the old .rc.obj in place and the
# resource silently survives the rebuild. For the manifest that is the ADR-4
# failure mode exactly -- zones that never appear, no message -- and for the icon
# it is a user looking at the previous artwork after regenerating it.
# OBJECT_DEPENDS is the generator-agnostic way to say it, and both toolchains
# build with Ninja (locally and on CI).
set_property(SOURCE starfish.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/starfish.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/starfish.ico")

# --- interface language (F-036 / T-014b) ------------------------------------
# The French catalogue is *built*, never committed as a binary: `lrelease` turns
# src/i18n/starfish_fr.ts into starfish_fr.qm next to the executable, which is
# the first place core::i18n looks. Guarded exactly like windeployqt below -- a
# Qt installation without LinguistTools cannot build a catalogue, and the
# application then stays in English (the source language) rather than failing to
# build over a translation.
#
# The `.qm` is also what --product-probe loads: a build that ships the catalogue
# is verified from the product's own window instead of being trusted.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(TARGET Qt6::lrelease)
    # Session 68: every src/i18n/starfish_<lang>.ts is a language. Adding one is
    # adding the file (docs/traduction.md): it is compiled here, placed next to
    # the executables, and core::i18n::availableLanguages() offers it.
    file(GLOB starfish_ts_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/i18n/starfish_*.ts)
    set(starfish_placed_qm_files)
    foreach(starfish_ts_file IN LISTS starfish_ts_files)
        get_filename_component(starfish_ts_name ${starfish_ts_file} NAME_WE)
        set(starfish_qm_file ${CMAKE_CURRENT_BINARY_DIR}/${starfish_ts_name}.qm)
        add_custom_command(
            OUTPUT ${starfish_qm_file}
            COMMAND Qt6::lrelease ${starfish_ts_file} -qm ${starfish_qm_file}
            DEPENDS ${starfish_ts_file}
            COMMENT "Compiling the catalogue ${starfish_ts_name} (F-036)"
            VERBATIM)
        # Next to the executables (application and tests): where core::i18n
        # looks first. Its own rule with the `.qm` as input -- not a POST_BUILD
        # step, which fires only on a relink (a measured trap, session 40s).
        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/${starfish_ts_name}.qm
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${starfish_qm_file} ${CMAKE_BINARY_DIR}/${starfish_ts_name}.qm
            DEPENDS ${starfish_qm_file}
            COMMENT "Placing ${starfish_ts_name}.qm next to the executables"
            VERBATIM)
        list(APPEND starfish_placed_qm_files ${CMAKE_BINARY_DIR}/${starfish_ts_name}.qm)
    endforeach()
    add_custom_target(starfish_translations ALL DEPENDS ${starfish_placed_qm_files})
    add_dependencies(starfish starfish_translations)
    # `cmake --build build --target starfish_lupdate`: refreshes every catalogue
    # from the sources (new strings appear as unfinished).
    if(TARGET Qt6::lupdate)
        add_custom_target(starfish_lupdate
            COMMAND Qt6::lupdate -no-obsolete -locations relative ${CMAKE_CURRENT_SOURCE_DIR}
                    -ts ${starfish_ts_files}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Updating the translation sources"
            VERBATIM)
    endif()
else()
    message(STATUS "Qt6::lrelease not found: no translation is built, the interface stays in English")
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
        # Qt's *own* strings are deployed on purpose (T-014b): F-036 is about the
        # interface, and the toolkit's dialogs are part of it -- the rename box, the
        # colour picker and their OK/Cancel are Qt's, and they only follow our
        # language if `translations/qtbase_<lang>.qm` travels with the binary. This
        # used to be `--no-translations`.
        add_custom_command(TARGET starfish POST_BUILD
            COMMAND "${WINDEPLOYQT_EXE}" --release
                    --no-system-d3d-compiler --no-opengl-sw
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
    #   oleaut32 VARIANT for IShellWindows::FindWindowSW (ADR-21)
    #   comctl32 SetWindowSubclass: shell submenus filled on WM_INITMENUPOPUP (ADR-21)
    #   shlwapi  StrRetToBufW (ADR-21)
    user32
    shell32
    ole32
    oleaut32
    uuid
    dwmapi
    comctl32
    shlwapi
    advapi32
    gdi32
)
