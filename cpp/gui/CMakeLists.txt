include(FetchContent)

FetchContent_Declare(imgui
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui"
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_executable(hardcopy-gui WIN32
    main.cpp
    companion_server.cpp
    localsend_client.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)

target_include_directories(hardcopy-gui PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../hardcopy/include
)

target_link_libraries(hardcopy-gui PRIVATE
    opengl32 dwmapi shell32 advapi32 gdi32 comdlg32 ole32 oleaut32
    winhttp bcrypt iphlpapi ws2_32 nlohmann_json::nlohmann_json
)

target_compile_features(hardcopy-gui PRIVATE cxx_std_17)
if(HARDCOPY_TEXT_MODE)
    target_compile_definitions(hardcopy-gui PRIVATE HARDCOPY_HAS_TEXT_MODE=1)
endif()

if(TARGET hardcopy-cli)
    add_dependencies(hardcopy-gui hardcopy-cli)
endif()
