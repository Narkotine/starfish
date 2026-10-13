configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

qt_add_executable(starfish WIN32
    main.cpp
    core/rules.cpp
    core/jsonstore.cpp
    core/zorder.cpp
    platform/win/desktop_host.cpp
    ui/fence_view.cpp
)

target_include_directories(starfish PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(starfish PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    # Win32 desktop integration (Progman, SHELLDLL_DefView, layered windows).
    user32
)
