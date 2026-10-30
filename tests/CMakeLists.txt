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

# The crash-repair marker (T-046): the note that lets a later run give the
# native desktop icons back after a hard kill. Pure Qt Core -- a file, and the
# rule that its *presence* alone decides.
add_executable(test_starfish_icon_hiding_marker
    test_icon_hiding_marker.cpp
    ../src/core/icon_hiding_marker.cpp
)

target_include_directories(test_starfish_icon_hiding_marker PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_icon_hiding_marker PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_icon_hiding_marker COMMAND test_starfish_icon_hiding_marker)

# The desktop backdrop (T-039 / ADR-9 option A): which entries a fence does NOT
# hold, and where the icons StarFish draws itself land. Pure -- no Win32, no
# window, so the two rules that matter (the free set is *computed*, and the
# layout is column-major like the Windows desktop) are unit-tested anywhere.
add_executable(test_starfish_backdrop_layout
    test_backdrop_layout.cpp
    ../src/core/desktop_item.cpp
)

target_include_directories(test_starfish_backdrop_layout PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_backdrop_layout PRIVATE Qt6::Core Qt6::Test)

add_test(NAME ui_backdrop_layout COMMAND test_starfish_backdrop_layout)

# The empty desktop's gestures (T-041 / F-001, F-022): a drag on the bare desktop
# draws a fence, a double-click hides or shows them all -- and, above all, what
# must NOT create one (a click, a wobble, a press on an icon, a draw too small to
# be a fence). Pure -- no Win32, no window, no mouse.
add_executable(test_starfish_backdrop_gesture
    test_backdrop_gesture.cpp
    ../src/core/desktop_item.cpp
)

target_include_directories(test_starfish_backdrop_gesture PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_backdrop_gesture PRIVATE Qt6::Core Qt6::Test)

add_test(NAME ui_backdrop_gesture COMMAND test_starfish_backdrop_gesture)

# The fence materials (T-057 / F-038): what a preset name means, and the
# #AARRGGBB arithmetic that applies one without rewriting the rest of the colour
# the document holds. Pure -- and checked against QColor itself, because the
# schema's colour form is defined by the toolkit that reads it back.
add_executable(test_starfish_fence_material
    test_fence_material.cpp
)

target_include_directories(test_starfish_fence_material PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_fence_material PRIVATE Qt6::Core Qt6::Gui Qt6::Test)

add_test(NAME ui_fence_material COMMAND test_starfish_fence_material)

# The fence's own context menu (T-076 / T-064): what it offers, in what order, and
# which radio mark is set. Pure -- a menu described as data, so the rules that
# matter (one entry per action, the check mark follows the document, the roll entry
# names the gesture) are checked with no window and no mouse.
add_executable(test_starfish_fence_menu
    test_fence_menu.cpp
)

target_include_directories(test_starfish_fence_menu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_fence_menu PRIVATE Qt6::Core Qt6::Test)

add_test(NAME ui_fence_menu COMMAND test_starfish_fence_menu)

# The interface language (F-036 / T-014b): which language a launch speaks, what a
# request for a language the product does not have does, and the fact that the
# source language needs no catalogue. Pure Qt Core, and deliberately *not* the
# compiled catalogue: that artifact is checked where it matters -- next to the
# executable, by --product-probe, which asks the product's own window.
add_executable(test_starfish_i18n
    test_i18n.cpp
    ../src/core/i18n.cpp
)

target_include_directories(test_starfish_i18n PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_i18n PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_i18n COMMAND test_starfish_i18n)

# The drawn right-click menu (T-092): the Windows 11 profile applied to the
# desktop's real menus (shapes measured on this machine) and the user's
# configuration -- hide, reorder, bring back, add -- as data, no window.
add_executable(test_starfish_context_menu
    test_context_menu.cpp
    ../src/core/context_menu.cpp
)

target_include_directories(test_starfish_context_menu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_starfish_context_menu PRIVATE Qt6::Core Qt6::Test)

add_test(NAME core_context_menu COMMAND test_starfish_context_menu)
