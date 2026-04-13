cmake_minimum_required(VERSION 3.24)

project(
    ScintillaQuick
    VERSION 0.1.0
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
# CMAKE_AUTOUIC is intentionally NOT enabled: the project contains no .ui
# files, so running `uic` on every build is pure overhead.
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Qt 6.5 is the oldest LTS line in wide use and covers everything this
# library relies on (QQuickItem, QSGNode, QSGTextNode, QSGImageNode,
# updatePaintNode, inputMethodQuery). If you bump the floor, please
# document the specific API that requires it.
find_package(
    Qt6 6.5 REQUIRED
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

add_library(
    ScintillaQuick STATIC
    include/scintillaquick/ScintillaQuickItem.h
    # Deprecated compatibility header for the old
    # `include/scintilla_quick/` path and `scintilla_quick::` namespace.
    # Listed here for IDE visibility only; the file is a thin shim that
    # re-exports the canonical constants from
    # `scintillaquick::version_*`.
    include/scintilla_quick/scintilla_quick.h
    src/core/ScintillaQuickCore.cpp
    src/core/scintillaquick_dispatch_table.h
    src/core/ScintillaQuickCore.h
    src/platform/scintillaquick_platqt.cpp
    src/platform/scintillaquick_platqt.h
    src/render/scintillaquick_scene_graph_renderer.cpp
    src/render/scintillaquick_scene_graph_renderer.h
    src/public/ScintillaQuickItem.cpp
    ${scintillaquick_scintilla_sources})

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

target_compile_definitions(
    ScintillaQuick
    PUBLIC
        SCINTILLAQUICK_STATIC_DEFINE=1)

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
        # include ScintillaQuickCore.h (the core .cpp itself and the
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

if(BUILD_TESTING)
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
