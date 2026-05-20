cmake_minimum_required(VERSION 3.10)
project(MediaPlayer)

# Use modern C++
set(CMAKE_CXX_STANDARD 17)

# Find FFmpeg using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswresample)


# --- DAEMON TARGET ---
add_executable(player Main.cpp)
target_link_libraries(player PRIVATE ${FFMPEG_LIBRARIES} m pthread dl)
target_include_directories(player PRIVATE ${FFMPEG_INCLUDE_DIRS})

# --- CONTROLLER CLIENT TARGET ---
add_executable(player-ctl player_ctl.cpp)
# (Note: player-ctl doesn't link FFmpeg or miniaudio, keeping it blindingly fast)
target_link_libraries(player-ctl PRIVATE pthread)
