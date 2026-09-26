include_guard(GLOBAL)

# Consumers opt in to Lexilla; the editor itself has no lexer dependency.
function(scintillaquick_add_lexilla)
    cmake_parse_arguments(PARSE_ARGV 0 lexilla "" "SOURCE_DIR" "")
    if(TARGET Lexilla::Lexilla)
        return()
    endif()
    if("${lexilla_SOURCE_DIR}" STREQUAL "")
        set(lexilla_SOURCE_DIR "${FETCHCONTENT_SOURCE_DIR_LEXILLA}")
    endif()
    if(NOT "${lexilla_SOURCE_DIR}" STREQUAL "" AND
            NOT EXISTS "${lexilla_SOURCE_DIR}/src/Lexilla.cxx")
        message(FATAL_ERROR "Lexilla source checkout is incomplete: ${lexilla_SOURCE_DIR}")
    endif()
    if("${lexilla_SOURCE_DIR}" STREQUAL "")
        find_package(Lexilla CONFIG QUIET)
        if(TARGET Lexilla::Lexilla)
            return()
        endif()
        include(FetchContent)
        FetchContent_Declare(lexilla
            GIT_REPOSITORY https://github.com/ScintillaOrg/lexilla.git
            GIT_TAG master
            GIT_SHALLOW FALSE)
        FetchContent_GetProperties(lexilla)
        if(NOT lexilla_POPULATED)
            FetchContent_Populate(lexilla)
        endif()
    endif()
    if(NOT EXISTS "${lexilla_SOURCE_DIR}/src/Lexilla.cxx")
        message(FATAL_ERROR "Lexilla source checkout is incomplete: ${lexilla_SOURCE_DIR}")
    endif()
    file(REAL_PATH "${lexilla_SOURCE_DIR}" lexilla_source_dir)
    file(GLOB lexilla_sources CONFIGURE_DEPENDS
        "${lexilla_source_dir}/lexlib/*.cxx"
        "${lexilla_source_dir}/lexers/*.cxx")
    add_library(scintillaquick_lexilla STATIC
        "${lexilla_source_dir}/src/Lexilla.cxx" ${lexilla_sources})
    add_library(Lexilla::Lexilla ALIAS scintillaquick_lexilla)
    set_target_properties(scintillaquick_lexilla PROPERTIES
        EXPORT_NAME Lexilla
        POSITION_INDEPENDENT_CODE ON
        SCINTILLAQUICK_LEXILLA_SOURCE_DIR "${lexilla_source_dir}")
    target_compile_features(scintillaquick_lexilla PUBLIC cxx_std_20)
    target_compile_definitions(scintillaquick_lexilla PRIVATE _CRT_SECURE_NO_DEPRECATE=1)
    target_include_directories(scintillaquick_lexilla
        PUBLIC
            $<BUILD_INTERFACE:${lexilla_source_dir}/include>
            $<INSTALL_INTERFACE:include/lexilla>
        PRIVATE "${lexilla_source_dir}/lexlib" "${lexilla_source_dir}/lexers")
    target_link_libraries(scintillaquick_lexilla PUBLIC ScintillaQuick::ScintillaQuick)
    if(MSVC)
        target_compile_options(scintillaquick_lexilla PRIVATE /W0)
    else()
        target_compile_options(scintillaquick_lexilla PRIVATE -w)
    endif()

    include(GNUInstallDirs)
    install(TARGETS scintillaquick_lexilla EXPORT ScintillaQuickLexillaTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY "${lexilla_source_dir}/include/"
        DESTINATION include/lexilla FILES_MATCHING PATTERN "*.h")
    install(EXPORT ScintillaQuickLexillaTargets
        FILE LexillaTargets.cmake NAMESPACE Lexilla::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Lexilla")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/LexillaConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lexilla-package/LexillaConfig.cmake" COPYONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lexilla-package/LexillaConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Lexilla")
endfunction()
