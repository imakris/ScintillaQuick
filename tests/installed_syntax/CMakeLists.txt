cmake_minimum_required(VERSION 3.24)
project(scintillaquick_installed_syntax LANGUAGES CXX)

find_package(ScintillaQuick CONFIG REQUIRED)
scintillaquick_add_syntax()
scintillaquick_add_syntax()
add_executable(scintillaquick_installed_syntax ../syntax/main.cpp)
target_link_libraries(scintillaquick_installed_syntax PRIVATE ScintillaQuick::Syntax)
enable_testing()
add_test(NAME scintillaquick_installed_syntax COMMAND scintillaquick_installed_syntax)
if(NOT WIN32)
    set_tests_properties(scintillaquick_installed_syntax PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
