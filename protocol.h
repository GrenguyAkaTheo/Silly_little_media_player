#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>

const char* const SOCKET_PATH = "/tmp/media_player.sock";

enum class CommandType : int32_t {
    ADD_TO_QUEUE     = 0,
    ADD_TO_PLAYLIST  = 1,
    PLAY             = 2,
    PAUSE            = 3,
    SKIP             = 4,
    SET_VOLUME       = 5,
    GET_STATUS       = 6,
    CLEAR_PLAYLIST   = 7,
    SHUFFLE_PLAYLIST = 8,
    PREVIOUS_TRACK   = 9,
    TOGGLE_SHUFFLE   = 10,
    TOGGLE_LOOP      = 11
};

// Packed response payload sent from Daemon back to player-ctl
struct __attribute__((packed)) PlayerStatusResponse {
    int32_t current_volume;
    int32_t is_paused;
    int32_t user_queue_size;
    int32_t playlist_pool_size;
    int32_t playlist_master_size;
    int32_t elapsed_seconds;      // NEW: Total elapsed playback seconds
    int32_t duration_seconds;     // NEW: Total length of track in seconds
    char track_name[512];
    bool shuffle_enabled;
    bool loop_enabled;
};

struct __attribute__((packed)) PlayerCommand {
    CommandType type;
    int32_t int_value;
    char path_value[512];
};

#endif // PROTOCOL_H
