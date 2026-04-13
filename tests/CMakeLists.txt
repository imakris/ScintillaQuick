if(WIN32 AND TARGET Qt6::qmake)
    get_target_property(scintillaquick_qmake_location Qt6::qmake IMPORTED_LOCATION)
    if(NOT scintillaquick_qmake_location)
        get_target_property(scintillaquick_qmake_location Qt6::qmake IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT scintillaquick_qmake_location)
        get_target_property(scintillaquick_qmake_location Qt6::qmake IMPORTED_LOCATION_DEBUG)
    endif()

    if(scintillaquick_qmake_location)
        get_filename_component(scintillaquick_qt_bin_dir "${scintillaquick_qmake_location}" DIRECTORY)
        get_filename_component(scintillaquick_qt_prefix_dir "${scintillaquick_qt_bin_dir}/.." ABSOLUTE)
        set(scintillaquick_qt_plugin_dir "${scintillaquick_qt_prefix_dir}/plugins")
    endif()
endif()

function(scintillaquick_set_test_environment test_name target_name extra_environment)
    if(WIN32)
        set(test_environment
            "QT_QPA_PLATFORM=windows${extra_environment}")

        if(scintillaquick_qt_plugin_dir)
            string(APPEND test_environment
                ";QT_PLUGIN_PATH=${scintillaquick_qt_plugin_dir}"
                ";QT_QPA_PLATFORM_PLUGIN_PATH=${scintillaquick_qt_plugin_dir}/platforms")
            set(test_path_modification
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:${target_name}>;PATH=path_list_prepend:${scintillaquick_qt_bin_dir}")
        else()
            string(APPEND test_environment
                ";QT_PLUGIN_PATH=$<TARGET_FILE_DIR:${target_name}>"
                ";QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:${target_name}>/platforms")
            set(test_path_modification
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:${target_name}>")
        endif()

        set_tests_properties(
            ${test_name}
            PROPERTIES
                ENVIRONMENT "${test_environment}"
                ENVIRONMENT_MODIFICATION "${test_path_modification}")
    else()
        set_tests_properties(
            ${test_name}
            PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen${extra_environment}")
    endif()
endfunction()

function(scintillaquick_register_validation_test test_name target_name extra_environment)
    add_test(
        NAME ${test_name}
        COMMAND ${target_name})

    scintillaquick_set_test_environment(
        ${test_name}
        ${target_name}
        "${extra_environment}")
endfunction()

add_executable(
    scintillaquick_smoke_test
    smoke/main.cpp)

target_include_directories(
    scintillaquick_smoke_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../examples/common)

target_link_libraries(
    scintillaquick_smoke_test
    PRIVATE
        ScintillaQuick::ScintillaQuick
        Qt6::Core
        Qt6::Gui
        Qt6::Quick)

scintillaquick_register_validation_test(
    scintillaquick_smoke_test
    scintillaquick_smoke_test
    "")

# --- Dispatch table unit tests -------------------------------------------
#
# Pure-function tests against the scene-graph update dispatch table at
# src/core/scintillaquick_dispatch_table.h. No Qt event loop required.
# This is the first piece of the "move correctness-critical logic into
# unit-testable helpers" direction called out in the review notes.

add_executable(
    scintillaquick_dispatch_table_test
    dispatch_table/main.cpp)

target_include_directories(
    scintillaquick_dispatch_table_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/scintilla/include)

# Deliberately link only Qt6::Core here: the dispatch table unit tests do
# NOT create QQuickWindow or touch the scene graph, so Gui/Quick are not
# needed at link time. If future tests in this target grow into Qt Quick
# territory, add Qt6::Gui / Qt6::Quick here and re-register with
# scintillaquick_register_validation_test (which sets QT_QPA_PLATFORM).
target_link_libraries(
    scintillaquick_dispatch_table_test
    PRIVATE
        ScintillaQuick::ScintillaQuick
        Qt6::Core)

add_test(
    NAME scintillaquick_dispatch_table_test
    COMMAND scintillaquick_dispatch_table_test)

# --- Review capture utility ---

add_executable(
    scintillaquick_review_capture
    review_capture/main.cpp)

target_include_directories(
    scintillaquick_review_capture
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${CMAKE_CURRENT_SOURCE_DIR}/../examples/common
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/render)

target_link_libraries(
    scintillaquick_review_capture
    PRIVATE
        ScintillaQuick::ScintillaQuick
        Qt6::Core
        Qt6::Gui
        Qt6::Quick)

# --- Benchmark-backed validation ---
#
# The embedded benchmark executable is declared at the top-level
# CMakeLists.txt and is gated on SCINTILLAQUICK_BUILD_BENCHMARKS. If
# benchmarks are disabled, skip registering the test rather than
# failing CMake with "target does not exist".
if(TARGET scintillaquick_embedded_benchmark)
    scintillaquick_register_validation_test(
        scintillaquick_embedded_benchmark
        scintillaquick_embedded_benchmark
        ";QT_FONT_DPI=96;QT_SCALE_FACTOR=1;QT_ENABLE_HIGHDPI_SCALING=0")
endif()

# --- Phase 2 frame validation (structural + semantic) ---

add_executable(
    scintillaquick_frame_validation_test
    frame_validation/main.cpp)

target_include_directories(
    scintillaquick_frame_validation_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${CMAKE_CURRENT_SOURCE_DIR}/../examples/common
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/render)

target_link_libraries(
    scintillaquick_frame_validation_test
    PRIVATE
        ScintillaQuick::ScintillaQuick
        Qt6::Core
        Qt6::Gui
        Qt6::Quick)

scintillaquick_register_validation_test(
    scintillaquick_frame_validation_test
    scintillaquick_frame_validation_test
    ";QT_FONT_DPI=96;QT_SCALE_FACTOR=1;QT_ENABLE_HIGHDPI_SCALING=0")

# --- Visual regression tests ---

set(VR_BASELINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/frame_visual_regression/baselines")
set(VR_ARTIFACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/visual_regression_artifacts")

add_executable(
    scintillaquick_visual_regression_test
    frame_visual_regression/main.cpp)

target_include_directories(
    scintillaquick_visual_regression_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../examples/common)

target_compile_definitions(
    scintillaquick_visual_regression_test
    PRIVATE
        BASELINE_DIR="${VR_BASELINE_DIR}"
        ARTIFACT_DIR="${VR_ARTIFACT_DIR}")

target_link_libraries(
    scintillaquick_visual_regression_test
    PRIVATE
        ScintillaQuick::ScintillaQuick
        Qt6::Core
        Qt6::Gui
        Qt6::Quick)

scintillaquick_register_validation_test(
    scintillaquick_visual_regression_test
    scintillaquick_visual_regression_test
    ";QT_FONT_DPI=96;QT_SCALE_FACTOR=1;QT_ENABLE_HIGHDPI_SCALING=0;SCINTILLAQUICK_VR_GENERATE_BASELINES=0")
