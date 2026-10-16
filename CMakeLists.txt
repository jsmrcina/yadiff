cmake_minimum_required(VERSION 3.21)
project(yadiff VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()

# UI-independent core: git access and diff model (unit tested).
add_library(yadiffcore STATIC
    src/DiffModel.cpp src/DiffModel.h
    src/GitRepo.cpp src/GitRepo.h
)
target_include_directories(yadiffcore PUBLIC src)
target_link_libraries(yadiffcore PUBLIC Qt6::Core)

# Widgets: shared by the app and the GUI tests.
add_library(yadiffui STATIC
    src/CodeView.cpp src/CodeView.h
    src/DiffPane.cpp src/DiffPane.h
    src/FileList.cpp src/FileList.h
    src/MainWindow.cpp src/MainWindow.h
    src/PreferencesDialog.cpp src/PreferencesDialog.h
    src/Settings.cpp src/Settings.h
)
target_link_libraries(yadiffui PUBLIC yadiffcore Qt6::Widgets)
target_compile_definitions(yadiffui PRIVATE YADIFF_VERSION="${PROJECT_VERSION}")

qt_add_executable(yadiff src/main.cpp)
target_link_libraries(yadiff PRIVATE yadiffui)
target_compile_definitions(yadiff PRIVATE YADIFF_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(yadiffcore PRIVATE -Wall -Wextra)
    target_compile_options(yadiffui PRIVATE -Wall -Wextra)
    target_compile_options(yadiff PRIVATE -Wall -Wextra)
endif()

# On Windows this makes it a GUI app: no console window, and the shell returns immediately.
set_target_properties(yadiff PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)

install(TARGETS yadiff RUNTIME DESTINATION bin BUNDLE DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(test_diffmodel tests/test_diffmodel.cpp)
    target_link_libraries(test_diffmodel PRIVATE yadiffcore Qt6::Test)
    add_test(NAME test_diffmodel COMMAND test_diffmodel)

    qt_add_executable(test_gitrepo tests/test_gitrepo.cpp)
    target_link_libraries(test_gitrepo PRIVATE yadiffcore Qt6::Test)
    add_test(NAME test_gitrepo COMMAND test_gitrepo)

    qt_add_executable(test_navigation tests/test_navigation.cpp)
    target_link_libraries(test_navigation PRIVATE yadiffui Qt6::Test)
    add_test(NAME test_navigation COMMAND test_navigation)
    set_tests_properties(test_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config")
endif()
