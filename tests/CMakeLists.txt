add_executable(test_starfish_core
    test_rules.cpp
    ../src/core/rules.cpp
    # T-010: fileMetaFromItem() adapts a DesktopItem for the rule engine.
    ../src/core/desktop_item.cpp
)

target_include_directories(test_starfish_core PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_core PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_rules COMMAND test_starfish_core)

# Pure z-order verdict logic used by the desktop placement check (ADR-4).
add_executable(test_starfish_zorder
    test_zorder.cpp
    ../src/core/zorder.cpp
)

target_include_directories(test_starfish_zorder PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_zorder PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_zorder COMMAND test_starfish_zorder)

# Desktop entry ordering / merging: the pure half of T-004, no Win32 involved.
add_executable(test_starfish_desktop_item
    test_desktop_item.cpp
    ../src/core/desktop_item.cpp
)

target_include_directories(test_starfish_desktop_item PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_desktop_item PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_desktop_item COMMAND test_starfish_desktop_item)

# Icon grid geometry + selection + paint: the pure half of T-005, no Win32.
add_executable(test_starfish_icon_grid
    test_icon_grid.cpp
    ../src/core/desktop_item.cpp
    ../src/ui/icon_grid.cpp
)

target_include_directories(test_starfish_icon_grid PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_icon_grid PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)

add_test(NAME ui_icon_grid COMMAND test_starfish_icon_grid)

# Fence lifecycle geometry (T-007): handle hit-test, resize with a minimum
# size, edge/centre snapping, clamping. Pure -- no widget, no Win32.
add_executable(test_starfish_fence_geometry
    test_fence_geometry.cpp
)

target_include_directories(test_starfish_fence_geometry PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_fence_geometry PRIVATE Qt6::Core Qt6::Test)

add_test(NAME ui_fence_geometry COMMAND test_starfish_fence_geometry)

# Layout persistence (T-008): the versioned document, its atomic write, the
# debounced auto-save and the refusal to load a corrupted file. Pure Qt Core.
add_executable(test_starfish_layout_store
    test_layout_store.cpp
    ../src/core/jsonstore.cpp
    ../src/core/layout_store.cpp
)

target_include_directories(test_starfish_layout_store PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_layout_store PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_layout_store COMMAND test_starfish_layout_store)

# Desktop folder watching (T-010 / F-018): the trigger of the rule engine. The
# diff is pure, the wiring is exercised against a temporary directory.
add_executable(test_starfish_desktop_watcher
    test_desktop_watcher.cpp
    ../src/core/desktop_watcher.cpp
)

target_include_directories(test_starfish_desktop_watcher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_desktop_watcher PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_desktop_watcher COMMAND test_starfish_desktop_watcher)

# Folder portals (T-011 / F-024): what a portal lists, and the refusal to treat
# a file or a missing folder as a folder. Qt Core + the local filesystem.
add_executable(test_starfish_portal
    test_portal.cpp
    ../src/core/portal.cpp
    ../src/core/desktop_item.cpp
)

target_include_directories(test_starfish_portal PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_portal PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_portal COMMAND test_starfish_portal)

# Named snapshots (T-012 / F-027/F-028): save, restore, list, delete a layout
# under a name, and export/import it as a plain file. Pure Qt Core.
add_executable(test_starfish_snapshot
    test_snapshot.cpp
    ../src/core/snapshot.cpp
    ../src/core/jsonstore.cpp
)

target_include_directories(test_starfish_snapshot PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_snapshot PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_snapshot COMMAND test_starfish_snapshot)

# Multi-monitor model (T-014 / F-032): which screen a fence belongs to, where a
# stored rect comes back to, and snapping against every screen's edges instead
# of a single bounding box. Pure -- a mixed-DPI two-monitor arrangement is
# unit-tested on a machine that owns one monitor.
add_executable(test_starfish_screen_layout
    test_screen_layout.cpp
)

target_include_directories(test_starfish_screen_layout PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_screen_layout PRIVATE Qt6::Core Qt6::Test)

add_test(NAME ui_screen_layout COMMAND test_starfish_screen_layout)
