add_executable(test_starfish_core
    test_rules.cpp
    ../src/core/rules.cpp
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
