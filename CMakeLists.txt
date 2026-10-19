cmake_minimum_required(VERSION 3.21)
project(yadiff VERSION 0.12.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
qt_standard_project_setup()

# UI-independent core: git access and diff model (unit tested).
add_library(yadiffcore STATIC
    src/AdoAuth.cpp src/AdoAuth.h
    src/AdoPr.cpp src/AdoPr.h
    src/DiffFormat.cpp src/DiffFormat.h
    src/DiffModel.cpp src/DiffModel.h
    src/LineDiff.cpp src/LineDiff.h
    src/Comments.cpp src/Comments.h
    src/PatchFile.cpp src/PatchFile.h
    src/HexDiff.cpp src/HexDiff.h
    src/StructuralDiff.cpp src/StructuralDiff.h
    src/TableDiff.cpp src/TableDiff.h
    src/Scratch.cpp src/Scratch.h
    src/Session.cpp src/Session.h
    src/ShellMenu.cpp src/ShellMenu.h
    src/Difftool.cpp src/Difftool.h
    src/FileFilter.cpp src/FileFilter.h
    src/GitHubPr.cpp src/GitHubPr.h
    src/GitHubReview.cpp src/GitHubReview.h
    src/GitRepo.cpp src/GitRepo.h
    src/Http.cpp src/Http.h
    src/Updater.cpp src/Updater.h
    src/PrReview.cpp src/PrReview.h
    src/Report.cpp src/Report.h
    src/ReviewStore.cpp src/ReviewStore.h
    src/Search.cpp src/Search.h
    src/SecretStore.cpp src/SecretStore.h
    src/TextFile.cpp src/TextFile.h
)
target_include_directories(yadiffcore PUBLIC src)
target_link_libraries(yadiffcore PUBLIC Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(yadiffcore PRIVATE advapi32) # Credential Manager (CredReadW/CredWriteW)
endif()

# Widgets: shared by the app and the GUI tests.
add_library(yadiffui STATIC
    src/AdoLoginDialog.cpp src/AdoLoginDialog.h
    src/CodeView.cpp src/CodeView.h
    src/DiffPane.cpp src/DiffPane.h
    src/FileList.cpp src/FileList.h
    src/HistoryDialog.cpp src/HistoryDialog.h
    src/ImageDiffView.cpp src/ImageDiffView.h
    src/TableDiffView.cpp src/TableDiffView.h
    src/MainWindow.cpp src/MainWindow.h
    src/NewComparisonDialog.cpp src/NewComparisonDialog.h
    src/ComparisonArgs.cpp src/ComparisonArgs.h
    src/OverviewBar.cpp src/OverviewBar.h
    src/PreferencesDialog.cpp src/PreferencesDialog.h
    src/SearchBar.cpp src/SearchBar.h
    src/Settings.cpp src/Settings.h
    src/Syntax.cpp src/Syntax.h
    src/Theme.cpp src/Theme.h
)
qt_add_resources(yadiffui "syntax_themes" PREFIX "/yadiff-syntax/themes" BASE resources/syntax-themes
    FILES resources/syntax-themes/vscode-light-plus.theme resources/syntax-themes/vibrant-ink.theme)
target_link_libraries(yadiffui PUBLIC yadiffcore Qt6::Widgets Qt6::Concurrent)

# Syntax highlighting comes from KDE's KSyntaxHighlighting when it is installed
# (Arch: syntax-highlighting; Windows: KDE Craft or vcpkg). Without it yadiff still builds.
option(YADIFF_WITH_KSYNTAXHIGHLIGHTING "Use KSyntaxHighlighting for syntax highlighting" ON)
if(YADIFF_WITH_KSYNTAXHIGHLIGHTING)
    find_package(KF6SyntaxHighlighting QUIET)
endif()
if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(yadiffui PUBLIC KF6::SyntaxHighlighting)
    target_compile_definitions(yadiffui PRIVATE YADIFF_HAVE_KSYNTAX)
    message(STATUS "yadiff: syntax highlighting via KSyntaxHighlighting ${KF6SyntaxHighlighting_VERSION}")
else()
    message(STATUS "yadiff: KSyntaxHighlighting not found, building without syntax highlighting")
endif()
target_compile_definitions(yadiffui PRIVATE YADIFF_VERSION="${PROJECT_VERSION}")

set(YADIFF_ICON_SIZES 16 24 32 48 64 128 256)
set(YADIFF_ICON_FILES "")
foreach(size IN LISTS YADIFF_ICON_SIZES)
    list(APPEND YADIFF_ICON_FILES resources/yadiff-${size}.png)
endforeach()

if(WIN32)
    qt_add_executable(yadiff src/main.cpp resources/yadiff.rc)
else()
    qt_add_executable(yadiff src/main.cpp)
endif()
target_link_libraries(yadiff PRIVATE yadiffui)
qt_add_resources(yadiff "icons" PREFIX "/icons" BASE resources FILES ${YADIFF_ICON_FILES})
target_compile_definitions(yadiff PRIVATE YADIFF_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(yadiffcore PRIVATE -Wall -Wextra)
    target_compile_options(yadiffui PRIVATE -Wall -Wextra)
    target_compile_options(yadiff PRIVATE -Wall -Wextra)
endif()

# On Windows this makes it a GUI app: no console window, and the shell returns immediately.
set_target_properties(yadiff PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)

install(TARGETS yadiff RUNTIME DESTINATION bin BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    install(FILES resources/yadiff.desktop DESTINATION share/applications)
    install(FILES resources/yadiff.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(test_diffmodel tests/test_diffmodel.cpp)
    target_link_libraries(test_diffmodel PRIVATE yadiffcore Qt6::Test)
    add_test(NAME test_diffmodel COMMAND test_diffmodel)

    qt_add_executable(test_gitrepo tests/test_gitrepo.cpp)
    target_link_libraries(test_gitrepo PRIVATE yadiffcore Qt6::Test)
    add_test(NAME test_gitrepo COMMAND test_gitrepo)
    set_tests_properties(test_gitrepo PROPERTIES ENVIRONMENT "YADIFF_NO_TRASH=1")

    qt_add_executable(test_format tests/test_format.cpp)
    target_link_libraries(test_format PRIVATE yadiffcore Qt6::Test)
    add_test(NAME test_format COMMAND test_format)

    qt_add_executable(test_ado tests/test_ado.cpp)
    target_link_libraries(test_ado PRIVATE yadiffcore Qt6::Test)
    target_compile_definitions(test_ado PRIVATE YADIFF_BINARY="$<TARGET_FILE:yadiff>")
    add_dependencies(test_ado yadiff)
    add_test(NAME test_ado COMMAND test_ado)

    qt_add_executable(test_update tests/test_update.cpp)
    target_link_libraries(test_update PRIVATE yadiffcore Qt6::Test)
    add_test(NAME test_update COMMAND test_update)

    qt_add_executable(test_navigation tests/test_navigation.cpp)
    target_link_libraries(test_navigation PRIVATE yadiffui Qt6::Test)
    add_test(NAME test_navigation COMMAND test_navigation)
    set_tests_properties(test_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;YADIFF_SYNCHRONOUS=1;YADIFF_NO_TRASH=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config")
endif()
