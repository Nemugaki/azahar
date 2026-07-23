cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(render_debugger LANGUAGES CXX)
    include(CTest)
endif()

add_library(render_debugger STATIC
    capture_file.cpp
    render_session.cpp
    include/render_debugger/capture_file.h
    include/render_debugger/render_session.h
)

target_compile_features(render_debugger PUBLIC cxx_std_20)
target_include_directories(render_debugger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
)

if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(render_debugger_test tests.cpp)
    target_link_libraries(render_debugger_test PRIVATE render_debugger)
    add_test(NAME render_debugger_test COMMAND render_debugger_test)
endif()

option(RENDER_DEBUGGER_BUILD_QT "Build the capture-only Qt viewer" OFF)
if(RENDER_DEBUGGER_BUILD_QT OR TARGET Qt6::Widgets)
    if(NOT TARGET Qt6::Widgets)
        find_package(Qt6 REQUIRED COMPONENTS Widgets)
    endif()
    add_library(render_debugger_qt STATIC
        qt/capture_viewer.cpp
        qt/capture_viewer.h
    )
    target_compile_features(render_debugger_qt PUBLIC cxx_std_20)
    target_include_directories(render_debugger_qt PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/qt>
    )
    target_link_libraries(render_debugger_qt PUBLIC render_debugger Qt6::Widgets)
    set_target_properties(render_debugger_qt PROPERTIES AUTOMOC ON)
    if(BUILD_TESTING AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        add_executable(render_debugger_qt_test qt/tests.cpp)
        target_link_libraries(render_debugger_qt_test PRIVATE render_debugger_qt)
        add_test(NAME render_debugger_qt_test COMMAND render_debugger_qt_test)
        set_tests_properties(render_debugger_qt_test PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
