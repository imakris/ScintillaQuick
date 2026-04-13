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

scintillaquick_register_validation_test(
    scintillaquick_embedded_benchmark
    scintillaquick_embedded_benchmark
    ";QT_FONT_DPI=96;QT_SCALE_FACTOR=1;QT_ENABLE_HIGHDPI_SCALING=0")

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
