cmake_minimum_required(VERSION 3.21)
project(curf VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets Network)
qt_standard_project_setup()

set(CURF_PLATFORM_ICON)
if(APPLE)
    set(CURF_PLATFORM_ICON assets/curf.icns)
    set_source_files_properties(assets/curf.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(WIN32)
    set(CURF_PLATFORM_ICON assets/curf.rc)
endif()

qt_add_executable(curf
    src/qt/main.cpp
    src/control_server.cpp
    src/url_util.cpp
    ${CURF_PLATFORM_ICON}
)

qt_add_resources(curf "icons" PREFIX "/" FILES assets/icon-256.png)

target_link_libraries(curf PRIVATE Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network)
if(WIN32)
    target_link_libraries(curf PRIVATE ws2_32)
endif()

set_target_properties(curf PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.curf.browser
    MACOSX_BUNDLE_BUNDLE_NAME curf
    MACOSX_BUNDLE_ICON_FILE curf.icns
)

if(UNIX AND NOT APPLE)
    install(FILES assets/icon.png DESTINATION share/icons/hicolor/1024x1024/apps RENAME curf.png)
    install(FILES assets/curf.desktop DESTINATION share/applications)
endif()

install(TARGETS curf BUNDLE DESTINATION . RUNTIME DESTINATION bin)
qt_generate_deploy_app_script(TARGET curf OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
