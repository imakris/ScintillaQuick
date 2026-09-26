include_guard(GLOBAL)

# Available to source consumers and users of the installed core package.
function(scintillaquick_add_syntax)
    if(TARGET ScintillaQuick::Syntax)
        return()
    endif()
    scintillaquick_add_lexilla()

    set(syntax_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scintillaquick_syntax.cpp")
    if(NOT EXISTS "${syntax_source}")
        set(syntax_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/syntax/scintillaquick_syntax.cpp")
    endif()
    add_library(scintillaquick_syntax STATIC "${syntax_source}")
    add_library(ScintillaQuick::Syntax ALIAS scintillaquick_syntax)
    set_target_properties(scintillaquick_syntax PROPERTIES
        EXPORT_NAME Syntax
        POSITION_INDEPENDENT_CODE ON)
    target_compile_features(scintillaquick_syntax PUBLIC cxx_std_20)
    target_link_libraries(scintillaquick_syntax
        PUBLIC ScintillaQuick::ScintillaQuick
        PRIVATE Lexilla::Lexilla)

    include(GNUInstallDirs)
    install(TARGETS scintillaquick_syntax EXPORT ScintillaQuickSyntaxTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(EXPORT ScintillaQuickSyntaxTargets
        FILE ScintillaQuickSyntaxTargets.cmake NAMESPACE ScintillaQuick::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScintillaQuick")
endfunction()
