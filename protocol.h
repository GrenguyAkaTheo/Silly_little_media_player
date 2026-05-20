#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>

// Fixed path to our local Unix socket node file
const char* const SOCKET_PATH = "/tmp/media_player.sock";

enum class CommandType : int32_t {
    ADD_TO_QUEUE = 0,
    PLAY         = 1,
    PAUSE        = 2,
    SKIP         = 3,
    SET_VOLUME   = 4,
    GET_STATUS   = 5
};

// CRITICAL: Packed structure ensures no hidden alignment padding bytes are added by the compiler
struct __attribute__((packed)) PlayerCommand {
    CommandType type;
    int32_t int_value;         // e.g., Volume level or status codes
    char path_value[512];      // Track paths
};

#endif // PROTOCOL_H
