#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "protocol.h"

void print_usage(const char* prog_name) {
    std::cout << "Usage:\n"
    << "  " << prog_name << " --add <path_to_song> ~~ Add a song to the queue\n"
    << "  " << prog_name << " --play ~~~~~~~~~~~~~~~~ Resume playback\n"
    << "  " << prog_name << " --pause ~~~~~~~~~~~~~~~ Pause playback\n"
    << "  " << prog_name << " --skip ~~~~~~~~~~~~~~~~ Skip current track\n"
    << "  " << prog_name << " --volume <0-100> ~~~~~~ Set software volume level\n"
    << "  " << prog_name << " --status ~~~~~~~~~~~~~~ Query current player state\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    PlayerCommand cmd{};
    std::string flag = argv[1];

    // 1. Parse Command Line Arguments into our protocol structure
    if (flag == "--add") {
        if (argc < 3) {
            std::cerr << "Error: No file path declared. Add a file path after --add.\n";
            return 1;
        }
        cmd.type = CommandType::ADD_TO_QUEUE;
        strncpy(cmd.path_value, argv[2], sizeof(cmd.path_value) - 1);
    }
    else if (flag == "--play") {
        cmd.type = CommandType::PLAY;
    }
    else if (flag == "--pause") {
        cmd.type = CommandType::PAUSE;
    }
    else if (flag == "--skip") {
        cmd.type = CommandType::SKIP;
    }
    else if (flag == "--volume") {
        if (argc < 3) {
            std::cerr << "Error: --volume requires a value (0-100).\n";
            return 1;
        }
        cmd.type = CommandType::SET_VOLUME;
        cmd.int_value = std::stoi(argv[2]);
        if (cmd.int_value < 0 || cmd.int_value > 100) {
            std::cerr << "Volume must be between 0 and 100.\n";
            return 1;
        }
    }
    else if (flag == "--status") {
        cmd.type = CommandType::GET_STATUS;
    }
    else {
        std::cerr << "Unknown command: " << flag << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // 2. Open a standard socket using the UNIX Local Domain protocol family
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << "CRITICAL: Failed to initialize system socket endpoint.\n";
        return 1;
    }

    sockaddr_un server_addr{};
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    // 3. Connect to the active daemon socket file
    std::cout << "Connecting to media engine at " << SOCKET_PATH << "...\n";
    if (connect(socket_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "CRITICAL ERROR: Player daemon is not currently running.\n"
        << "(Could not bridge socket channel connection)\n";
        close(socket_fd);
        return 1;
    }

    // 4. Blast the binary payload packet across the kernel bridge
    std::cout << "Sending command package...\n";
    if (send(socket_fd, &cmd, sizeof(PlayerCommand), 0) < 0) {
        std::cerr << "Error: Failed to transmit command bundle.\n";
        close(socket_fd);
        return 1;
    }

    std::cout << "Command sent cleanly!\n";

    close(socket_fd);
    return 0;
}
