add_library(azahar_debugger STATIC EXCLUDE_FROM_ALL
    capture_file.cpp
    capture_file.h
    render_session.cpp
    render_session.h
)

target_include_directories(azahar_debugger PUBLIC ${CMAKE_SOURCE_DIR}/src)

create_target_directory_groups(azahar_debugger)
