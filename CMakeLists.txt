cmake_minimum_required(VERSION 3.10)
project(MediaPlayer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Automatically handle Qt's Meta-Object Compiler (MOC)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# --- DEPENDENCIES ---
# Find FFmpeg using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswresample)

# Find Qt6 (or Qt5 depending on your system)
find_package(Qt6 COMPONENTS Core Gui Widgets Network REQUIRED)

# --- DAEMON TARGET ---
add_executable(Geniusnt Main.cpp)
target_link_libraries(Geniusnt PRIVATE ${FFMPEG_LIBRARIES} m pthread dl)
target_include_directories(Geniusnt PRIVATE ${FFMPEG_INCLUDE_DIRS})

# --- CONTROLLER CLIENT TARGET ---
add_executable(Geniusnt-ctl player_ctl.cpp)
target_link_libraries(Geniusnt-ctl PRIVATE pthread)

# --- GUI TARGET ---
add_executable(Geniusnt-gui
    gui_main.cpp
    main_window.cpp
    main_window.h
    player_client.cpp
    player_client.h
    protocol.h
)
target_link_libraries(Geniusnt-gui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
