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
