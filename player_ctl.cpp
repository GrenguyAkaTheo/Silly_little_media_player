#include <iostream>
#include <string>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "protocol.h"

namespace fs = std::filesystem;

// Helper utility to turn integer seconds into zero-padded MM:SS strings
std::string format_time(int32_t total_seconds) {
    int32_t minutes = total_seconds / 60;
    int32_t seconds = total_seconds % 60;
    std::string min_str = (minutes < 10 ? "0" : "") + std::to_string(minutes);
    std::string sec_str = (seconds < 10 ? "0" : "") + std::to_string(seconds);
    return min_str + ":" + sec_str;
}

// Helper function to handle sending a packed struct across a raw socket descriptor
bool transmit_command(const PlayerCommand& cmd) {
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) return false;

    sockaddr_un server_addr{};
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    if (connect(socket_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(socket_fd);
        return false;
    }

    send(socket_fd, &cmd, sizeof(PlayerCommand), 0);
    close(socket_fd);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "\nMedia player status;\n"
                << "============================================================\n"
                << "| Track   : " << res.track_name << "\n"
                << "| Time    : " << format_time(res.elapsed_seconds) << " / " << format_time(res.duration_seconds) << "\n"
                << "| Volume  : " << res.current_volume << "%\n"
                << "| Playing : " << (res.is_paused ? "PAUSED" : "PLAYING") << "\n"
                << "| Shuffle : " << (res.shuffle_enabled ? "ON" : "OFF") << "\n"
                << "| Loop    : " << (res.loop_enabled ? "ON" : "OFF") << "\n"
                << "|-----------------------------------------------------------\n"
                << "| User Queue     : " << res.user_queue_size << " tracks remaining\n"
                << "| Playlist Queue : " << res.playlist_pool_size << " tracks remaining\n"
                << "============================================================\n" << std::flush;
        return 1;

    }

    std::string flag = argv[1];

    if (flag == "--add") {
        if (argc < 3) return 1;
        PlayerCommand cmd{};
        cmd.type = CommandType::ADD_TO_QUEUE;
        strncpy(cmd.path_value, argv[2], sizeof(cmd.path_value) - 1);
        transmit_command(cmd);
    }
    else if (flag == "--playlist") {
        if (argc < 3) {
            std::cerr << "Error: --playlist requires a target path.\n";
            return 1;
        }
        std::string m3u_path_str = argv[2];

        // 1. Resolve the absolute path of the playlist and extract its parent directory
        fs::path m3u_path = fs::absolute(m3u_path_str);
        fs::path playlist_dir = m3u_path.parent_path();

        std::ifstream file(m3u_path);
        if (!file.is_open()) {
            std::cerr << "Error: Could not open playlist file: " << m3u_path_str << "\n";
            return 1;
        }

        std::string line;
        int tracks_loaded = 0;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            // 2. Determine if the track path is relative or absolute
            fs::path track_path(line);
            if (track_path.is_relative()) {
                // Combine the playlist's parent folder with the relative track filename
                track_path = playlist_dir / track_path;
            }

            // 3. Convert back to an absolute canonical string for the daemon
            std::string resolved_path = fs::absolute(track_path).string();

            PlayerCommand cmd{};
            cmd.type = CommandType::ADD_TO_PLAYLIST;
            strncpy(cmd.path_value, resolved_path.c_str(), sizeof(cmd.path_value) - 1);

            if (transmit_command(cmd)) {
                tracks_loaded++;
            }
            usleep(2000);
        }
        std::cout << "Successfully parsed playlist. Forwarded " << tracks_loaded << " resolved tracks to engine.\n";
    }
    else if (flag == "--clear") {
        PlayerCommand cmd{};
        cmd.type = CommandType::CLEAR_PLAYLIST;
        transmit_command(cmd);
    }
    else if (flag == "--shuffle") {
        PlayerCommand cmd{};
        cmd.type = CommandType::TOGGLE_SHUFFLE;
        transmit_command(cmd);
        std::cout << "Sent shuffle toggle command to media engine.\n";
    }
    else if (flag == "--loop") {
        PlayerCommand cmd{};
        cmd.type = CommandType::TOGGLE_LOOP;
        transmit_command(cmd);
        std::cout << "Sent loop toggle command to media engine.\n";
    }
    else if (flag == "--back") {
        PlayerCommand cmd{};
        cmd.type = CommandType::PREVIOUS_TRACK;
        if (transmit_command(cmd)) {
            std::cout << "Sent previous track request to media engine.\n";
        } else {
            std::cerr << "Error: Could not contact media engine daemon.\n";
        }
    }
    else if (flag == "--status") {
        PlayerCommand cmd{};
        cmd.type = CommandType::GET_STATUS;

        int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            std::cerr << "Error creating client socket descriptor.\n";
            return 1;
        }

        sockaddr_un server_addr{};
        server_addr.sun_family = AF_UNIX;
        strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

        if (connect(socket_fd, (sockaddr*)&server_addr, sizeof(server_addr)) >= 0) {
            // 1. Send our request token down the wire
            send(socket_fd, &cmd, sizeof(PlayerCommand), 0);

            // 2. Hold the socket open and read back the daemon's status structure pack
            PlayerStatusResponse res{};
            if (recv(socket_fd, &res, sizeof(PlayerStatusResponse), 0) > 0) {

                std::cout << "\nMedia player status;\n"
                << "============================================================\n"
                << " Track   : " << res.track_name << "\n"
                << " Time    : " << format_time(res.elapsed_seconds) << " / " << format_time(res.duration_seconds) << "\n"
                << " Volume  : " << res.current_volume << "%\n"
                << " Playing : " << (res.is_paused ? "PAUSED" : "PLAYING") << "\n"
                << " Shuffle : " << (res.shuffle_enabled ? "ON" : "OFF") << "\n"
                << " Loop    : " << (res.loop_enabled ? "ON" : "OFF") << "\n"
                << "------------------------------------------------------------\n"
                << " User Queue     : " << res.user_queue_size << " tracks remaining\n"
                << " Playlist Queue : " << res.playlist_pool_size << " tracks remaining\n"
                << "============================================================\n" << std::flush;
            }
            close(socket_fd);
            return 0;
        } else {
            std::cerr << "Error: Could not contact media engine daemon.\n";
            close(socket_fd);
            return 1;
        }
    }

    else {
        // Handle global control configurations (play, pause, skip, volume, status)
        PlayerCommand cmd{};
        if (flag == "--play") cmd.type = CommandType::PLAY;
        else if (flag == "--pause") cmd.type = CommandType::PAUSE;
        else if (flag == "--skip") cmd.type = CommandType::SKIP;
        else if (flag == "--volume" && argc >= 3) {
            cmd.type = CommandType::SET_VOLUME;
            cmd.int_value = std::stoi(argv[2]);
        }
        transmit_command(cmd);
    }

    return 0;
}
