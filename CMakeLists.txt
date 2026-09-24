cmake_minimum_required(VERSION 3.21)
project(curf VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets Network)
qt_standard_project_setup()

qt_add_executable(curf
    src/qt/main.cpp
    src/control_server.cpp
    src/url_util.cpp
)

target_link_libraries(curf PRIVATE Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network)
if(WIN32)
    target_link_libraries(curf PRIVATE ws2_32)
endif()

set_target_properties(curf PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.curf.browser
    MACOSX_BUNDLE_BUNDLE_NAME curf
)

install(TARGETS curf BUNDLE DESTINATION . RUNTIME DESTINATION bin)
qt_generate_deploy_app_script(TARGET curf OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
