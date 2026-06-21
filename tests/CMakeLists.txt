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
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:${target_name}>;PATH=path_list_prepend:$<TARGET_FILE_DIR:ScintillaQuick>;PATH=path_list_prepend:${scintillaquick_qt_bin_dir}")
        else()
            string(APPEND test_environment
                ";QT_PLUGIN_PATH=$<TARGET_FILE_DIR:${target_name}>"
                ";QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:${target_name}>/platforms")
            set(test_path_modification
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:${target_name}>;PATH=path_list_prepend:$<TARGET_FILE_DIR:ScintillaQuick>")
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

# Declare a Qt-Quick-using executable in this tests/ tree. Every such
# target needs tests/support and examples/common on the include path
# plus the Core/Gui/Quick Qt module triple. Additional include
# directories (e.g. src/core, src/render) can be passed positionally
# after the source file.
#
# Targets that deliberately link only a subset of Qt (e.g. the
# dispatch-table unit test, which is Core-only and never spins up a
# QQuickWindow) should NOT use this helper; declare them by hand.
function(scintillaquick_add_quick_test_target target_name source_file)
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${CMAKE_CURRENT_SOURCE_DIR}/../examples/common
        ${ARGN})
    target_link_libraries(${target_name} PRIVATE
        ScintillaQuick::ScintillaQuick
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick)
endfunction()

scintillaquick_add_quick_test_target(
    scintillaquick_smoke_test
    smoke/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render)

scintillaquick_register_validation_test(
    scintillaquick_smoke_test
    scintillaquick_smoke_test
    "")

scintillaquick_add_quick_test_target(
    scintillaquick_tortoisediff_step1_test
    tortoisediff_step1/main.cpp)

scintillaquick_register_validation_test(
    scintillaquick_tortoisediff_step1_test
    scintillaquick_tortoisediff_step1_test
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
        ${CMAKE_CURRENT_SOURCE_DIR}/support
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

# Audit-only Scintilla.iface inventory check. This does not compile or generate
# dispatch code; it reports drift between the vendored iface inventory and the
# checked-in manual rule table.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME scintillaquick_dispatch_iface_audit_test
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/dispatch/audit_dispatch_table.py
            --check
            --repo-root
            ${CMAKE_CURRENT_SOURCE_DIR}/..)
else()
    message(WARNING
        "Python3 interpreter not found; skipping "
        "scintillaquick_dispatch_iface_audit_test")
endif()

# --- Benchmark-backed validation ---
#
# The embedded benchmark executable is declared at the top-level
# CMakeLists.txt and is gated on SCINTILLAQUICK_BUILD_BENCHMARKS. If
# benchmarks are disabled, skip registering the test rather than
# failing CMake with "target does not exist".
if(TARGET scintillaquick_embedded_benchmark)
    add_test(
        NAME scintillaquick_embedded_benchmark
        COMMAND scintillaquick_embedded_benchmark
            -s vertical_scroll_step_latency_64
            -s vertical_scroll_page_step_latency_16
            -s vertical_scroll_bounce_latency_48
            -s vertical_wheel_bounce_latency_48
            -s wrapped_wheel_bounce_latency_24
            -s zoom_wheel_bounce_latency_24)

    scintillaquick_set_test_environment(
        scintillaquick_embedded_benchmark
        scintillaquick_embedded_benchmark
        ";QT_FONT_DPI=96;QT_SCALE_FACTOR=1;QT_ENABLE_HIGHDPI_SCALING=0")
endif()

# --- Phase 2 frame validation (structural + semantic) ---

scintillaquick_add_quick_test_target(
    scintillaquick_frame_validation_test
    frame_validation/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/render)

scintillaquick_register_validation_test(
    scintillaquick_frame_validation_test
    scintillaquick_frame_validation_test
    ";QT_FONT_DPI=96;QT_SCALE_FACTOR=1;QT_ENABLE_HIGHDPI_SCALING=0")

# --- Visual regression tests ---

set(VR_BASELINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/frame_visual_regression/baselines")
set(VR_ARTIFACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/visual_regression_artifacts")

scintillaquick_add_quick_test_target(
    scintillaquick_visual_regression_test
    frame_visual_regression/main.cpp)

target_compile_definitions(
    scintillaquick_visual_regression_test
    PRIVATE
        BASELINE_DIR="${VR_BASELINE_DIR}"
        ARTIFACT_DIR="${VR_ARTIFACT_DIR}")

scintillaquick_register_validation_test(
    scintillaquick_visual_regression_test
    scintillaquick_visual_regression_test
    ";QT_FONT_DPI=96;QT_SCALE_FACTOR=1;QT_ENABLE_HIGHDPI_SCALING=0;SCINTILLAQUICK_VR_GENERATE_BASELINES=0")
