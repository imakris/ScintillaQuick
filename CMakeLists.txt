cmake_minimum_required(VERSION 3.24)

project(
    ScintillaQuick
    VERSION 0.1.0
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# Project-level options. Each option has a deliberate default so that the
# common in-tree case (configure -> build everything) still works without
# the user having to know any of this exists.
option(SCINTILLAQUICK_BUILD_EXAMPLES
    "Build the example applications (scintillaquick_minimal_editor)" ON)
option(SCINTILLAQUICK_BUILD_BENCHMARKS
    "Build the benchmark applications (scintillaquick_embedded_benchmark)" ON)
option(SCINTILLAQUICK_BUILD_TESTS
    "Build the test suite (gated on BUILD_TESTING as well)" ON)
# Expose Scintilla's vendored sources as an OBJECT library instead of
# dropping them into the main target. Set to OFF only if you want a
# flatter single-target static build and are willing to inherit
# Scintilla's compile warnings into the glue layer.
option(SCINTILLAQUICK_USE_SCINTILLA_OBJECT_LIBRARY
    "Compile the vendored Scintilla sources as a separate OBJECT library" ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
# CMAKE_AUTOUIC is intentionally NOT enabled: the project contains no .ui
# files, so running `uic` on every build is pure overhead.

# Qt 6.7 is the effective minimum because the renderer and platform layer
# rely on QSGTextNode. If you bump the floor again, document the specific
# API that requires it, and update the variable below so that the
# generated package config inherits the new floor.
set(scintillaquick_qt6_minimum_version 6.7)

find_package(
    Qt6 ${scintillaquick_qt6_minimum_version} REQUIRED
    COMPONENTS
        Core
        Gui
        Qml
        Quick)

set(scintillaquick_scintilla_sources
    third_party/scintilla/src/AutoComplete.cxx
    third_party/scintilla/src/CallTip.cxx
    third_party/scintilla/src/CaseConvert.cxx
    third_party/scintilla/src/CaseFolder.cxx
    third_party/scintilla/src/CellBuffer.cxx
    third_party/scintilla/src/ChangeHistory.cxx
    third_party/scintilla/src/CharClassify.cxx
    third_party/scintilla/src/CharacterCategoryMap.cxx
    third_party/scintilla/src/CharacterType.cxx
    third_party/scintilla/src/ContractionState.cxx
    third_party/scintilla/src/DBCS.cxx
    third_party/scintilla/src/Decoration.cxx
    third_party/scintilla/src/Document.cxx
    third_party/scintilla/src/EditModel.cxx
    third_party/scintilla/src/EditView.cxx
    third_party/scintilla/src/Editor.cxx
    third_party/scintilla/src/Geometry.cxx
    third_party/scintilla/src/Indicator.cxx
    third_party/scintilla/src/KeyMap.cxx
    third_party/scintilla/src/LineMarker.cxx
    third_party/scintilla/src/MarginView.cxx
    third_party/scintilla/src/PerLine.cxx
    third_party/scintilla/src/PositionCache.cxx
    third_party/scintilla/src/RESearch.cxx
    third_party/scintilla/src/RunStyles.cxx
    third_party/scintilla/src/ScintillaBase.cxx
    third_party/scintilla/src/Selection.cxx
    third_party/scintilla/src/Style.cxx
    third_party/scintilla/src/UniConversion.cxx
    third_party/scintilla/src/UniqueString.cxx
    third_party/scintilla/src/ViewStyle.cxx
    third_party/scintilla/src/XPM.cxx)

# Optionally compile Scintilla as an OBJECT library so we can apply
# compile flags/warnings that should only affect the vendored code, not
# the ScintillaQuick glue layer. The OBJECT target is a private
# implementation detail (not exported, not installed).
if(SCINTILLAQUICK_USE_SCINTILLA_OBJECT_LIBRARY)
    add_library(scintillaquick_scintilla_objects OBJECT
        ${scintillaquick_scintilla_sources})
    if(BUILD_SHARED_LIBS)
        set_target_properties(scintillaquick_scintilla_objects PROPERTIES
            POSITION_INDEPENDENT_CODE ON)
    endif()
    # Scintilla relies on C++20.
    target_compile_features(scintillaquick_scintilla_objects PUBLIC cxx_std_20)
    target_include_directories(
        scintillaquick_scintilla_objects
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/scintilla/include
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/scintilla/src)
    target_compile_definitions(
        scintillaquick_scintilla_objects
        PRIVATE
            SCINTILLA_QT_QML=1
            MAKING_LIBRARY=1
            _CRT_SECURE_NO_DEPRECATE=1)
    # Scintilla must see the Qt Core headers (used by its Qt platform
    # bits in our glue layer) even though this OBJECT target itself
    # does not link Qt.
    target_link_libraries(scintillaquick_scintilla_objects
        PUBLIC Qt6::Core Qt6::Gui)
    if(MSVC)
        target_compile_options(scintillaquick_scintilla_objects PRIVATE /FS)
    endif()
    set(scintillaquick_scintilla_sources_for_main_target "")
    set(scintillaquick_scintilla_objects_for_main_target
        $<TARGET_OBJECTS:scintillaquick_scintilla_objects>)
else()
    set(scintillaquick_scintilla_sources_for_main_target
        ${scintillaquick_scintilla_sources})
    set(scintillaquick_scintilla_objects_for_main_target "")
endif()

# BUILD_SHARED_LIBS (CMake built-in) switches the library between static
# and shared. Default to static.
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF)
endif()

add_library(
    ScintillaQuick
    include/scintillaquick/scintillaquick_item.h
    src/core/scintillaquick_core.cpp
    src/core/scintillaquick_dispatch_table.h
    src/core/scintillaquick_core.h
    src/platform/scintillaquick_platqt.cpp
    src/platform/scintillaquick_platqt.h
    src/platform/scintillaquick_fonts.cpp
    src/platform/scintillaquick_fonts.h
    src/platform/scintillaquick_fonts.qrc
    src/render/scintillaquick_scene_graph_renderer.cpp
    src/render/scintillaquick_scene_graph_renderer.h
    src/public/scintillaquick_item.cpp
    ${scintillaquick_scintilla_sources_for_main_target}
    ${scintillaquick_scintilla_objects_for_main_target})

add_library(ScintillaQuick::ScintillaQuick ALIAS ScintillaQuick)

target_compile_features(ScintillaQuick PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ScintillaQuick PRIVATE /FS)
endif()

target_compile_definitions(
    ScintillaQuick
    PRIVATE
        SCINTILLA_QT_QML=1
        MAKING_LIBRARY=1
        _CRT_SECURE_NO_DEPRECATE=1)

# When building as a static library the SCINTILLAQUICK_EXPORT macro needs
# to resolve to an empty expansion on Windows. When building shared, the
# macro must expand to dllexport/dllimport on the library/consumer side
# respectively; the header already handles that via MAKING_LIBRARY.
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        ScintillaQuick
        PUBLIC
            SCINTILLAQUICK_STATIC_DEFINE=1)
endif()

target_include_directories(
    ScintillaQuick
    PUBLIC
        # Consumers get the main header and the Scintilla *public* API
        # (Scintilla.h, ScintillaTypes.h, ScintillaMessages.h,
        # ScintillaStructures.h) which the public header references.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/scintilla/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/scintillaquick/third_party/scintilla/include>
        # Scintilla's *internal* src/ headers (Debugging.h, Geometry.h,
        # Platform.h, Editor.h, ...) are only needed by in-tree TUs that
        # include scintillaquick_core.h (the core .cpp itself and the
        # test-support validation access). Expose them on the
        # BUILD_INTERFACE so those in-tree consumers compile, but deliberately
        # do NOT expose them on the INSTALL_INTERFACE: downstream consumers
        # should not need to know about Scintilla's internals to link the
        # installed package.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/scintilla/src>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/render
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/scintilla/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/scintilla/src)

target_link_libraries(
    ScintillaQuick
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
    PRIVATE
        Qt6::Qml)

set(scintillaquick_example_common_include_dir
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/common)

if(SCINTILLAQUICK_BUILD_EXAMPLES)
    add_executable(
        scintillaquick_minimal_editor
        examples/minimal_editor/main.cpp)

    target_include_directories(
        scintillaquick_minimal_editor
        PRIVATE
            ${scintillaquick_example_common_include_dir})

    target_link_libraries(
        scintillaquick_minimal_editor
        PRIVATE
            ScintillaQuick::ScintillaQuick
            Qt6::Core
            Qt6::Gui
            Qt6::Quick)
endif()

if(SCINTILLAQUICK_BUILD_BENCHMARKS)
    add_executable(
        scintillaquick_embedded_benchmark
        benchmarks/embedded_editor/main.cpp)

    target_include_directories(
        scintillaquick_embedded_benchmark
        PRIVATE
            ${scintillaquick_example_common_include_dir})

    target_link_libraries(
        scintillaquick_embedded_benchmark
        PRIVATE
            ScintillaQuick::ScintillaQuick
            Qt6::Core
            Qt6::Gui
            Qt6::Quick)
endif()

if(BUILD_TESTING AND SCINTILLAQUICK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

install(
    TARGETS ScintillaQuick
    EXPORT ScintillaQuickTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
        PATTERN "*.h")

install(
    DIRECTORY third_party/scintilla/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/scintillaquick/third_party/scintilla/include
    FILES_MATCHING
        PATTERN "*.h")

# Note: third_party/scintilla/src/ headers are deliberately NOT installed.
# They are Scintilla's internal implementation and should not be visible to
# downstream consumers of the installed package. See target_include_directories
# above for the BUILD_INTERFACE-only exposure used by in-tree tests.

set(scintillaquick_package_install_dir
    ${CMAKE_INSTALL_LIBDIR}/cmake/ScintillaQuick)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScintillaQuickConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ScintillaQuickConfig.cmake
    INSTALL_DESTINATION ${scintillaquick_package_install_dir})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ScintillaQuickConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(
    EXPORT ScintillaQuickTargets
    FILE ScintillaQuickTargets.cmake
    NAMESPACE ScintillaQuick::
    DESTINATION ${scintillaquick_package_install_dir})

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ScintillaQuickConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ScintillaQuickConfigVersion.cmake
    DESTINATION ${scintillaquick_package_install_dir})
