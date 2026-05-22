#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <algorithm>
#include <random>

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/audio_fifo.h>
    #include <libswresample/swresample.h>
}

#define MA_NO_ALSA
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "protocol.h"

struct AudioPlayerState {
    AVAudioFifo* fifo = nullptr;
    pthread_mutex_t mutex;

    std::vector<std::string> user_queue;
    std::vector<std::string> auto_playlist;
    std::vector<std::string> playlist_master;
    std::vector<std::string> history_queue;

    int current_volume = 80;
    bool is_paused = false;
    bool skip_requested = false;
    bool previous_requested = false;
    bool quit_requested = false;
    std::string current_track_name = "None";
    std::string current_track_path = "";

    // TIME TRACKING VARIABLES
    uint32_t sample_rate = 44100;
    double accumulated_samples = 0.0; // Total individual audio frames played for this track
    int32_t total_duration_seconds = 0;

    bool shuffle_enabled = false;
    bool loop_enabled = false;
};

void audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioPlayerState* state = (AudioPlayerState*)pDevice->pUserData;
    if (!state || !state->fifo) return;

    pthread_mutex_lock(&state->mutex);

    // 1. If paused, just output silence and do NOT touch the FIFO or the time tracker
    if (state->is_paused) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    // 2. If we don't have enough data yet, output silence and wait
    if (av_audio_fifo_size(state->fifo) < (int)frameCount) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    // 3. We are actively playing! Read frames from the FIFO buffer
    uint8_t* output_buffer = (uint8_t*)pOutput;
    av_audio_fifo_read(state->fifo, (void**)&output_buffer, frameCount);

    // 4. Get time stamp
    state->accumulated_samples += frameCount;

    float volume_factor = state->current_volume / 100.0f;
    pthread_mutex_unlock(&state->mutex);

    // 5. Apply software volume scaling
    int16_t* samples = (int16_t*)pOutput;
    int total_samples = frameCount * pDevice->playback.channels;
    for (int i = 0; i < total_samples; ++i) {
        samples[i] = (int16_t)(samples[i] * volume_factor);
    }
}

void socket_listener_thread(AudioPlayerState* state) {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return;

    unlink(SOCKET_PATH);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return;
    }

    listen(server_fd, 10);

    while (true) {
        pthread_mutex_lock(&state->mutex);
        if (state->quit_requested) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        pthread_mutex_unlock(&state->mutex);

        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        PlayerCommand cmd;
        if (recv(client_fd, &cmd, sizeof(PlayerCommand), 0) > 0) {
            pthread_mutex_lock(&state->mutex);

            switch (cmd.type) {
                case CommandType::ADD_TO_QUEUE:
                    state->user_queue.push_back(cmd.path_value);
                    std::cout << "\n[User Queue] Added: " << cmd.path_value << "\n" << std::flush;
                    break;
                case CommandType::ADD_TO_PLAYLIST:
                    // Store in BOTH the active pool and the permanent master copy
                    state->auto_playlist.push_back(cmd.path_value);
                    state->playlist_master.push_back(cmd.path_value);
                    break;
                case CommandType::CLEAR_PLAYLIST:
                    state->auto_playlist.clear();
                    state->playlist_master.clear(); // Wipe both out
                    std::cout << "\n[Playlist] Context cleared completely.\n" << std::flush;
                    break;
                case CommandType::TOGGLE_SHUFFLE:
                    state->shuffle_enabled = !state->shuffle_enabled;

                    if (state->shuffle_enabled) {
                        // Turning Shuffle ON: Randomize the remaining pool
                        if (!state->auto_playlist.empty()) {
                            std::random_device rd;
                            std::mt19937 g(rd());
                            std::shuffle(state->auto_playlist.begin(), state->auto_playlist.end(), g);
                        }
                        std::cout << "[Playlist] Shuffle toggled ON.\n" << std::flush;
                    } else {
                        // Turning Shuffle OFF: Restore original master order, but skip songs already played
                        state->auto_playlist.clear();
                        bool found_current = false;

                        for (const auto& track : state->playlist_master) {
                            if (track == state->current_track_path) {
                                found_current = true;
                                continue; // Skip the currently playing song
                            }
                            if (found_current) {
                                state->auto_playlist.push_back(track);
                            }
                        }
                        std::cout << "[Playlist] Shuffle toggled OFF. Restored original order sequence.\n" << std::flush;
                    }
                    break;

                    case CommandType::TOGGLE_LOOP:
                        state->loop_enabled = !state->loop_enabled;
                        std::cout << "[Playlist] Loop toggled " << (state->loop_enabled ? "ON" : "OFF") << ".\n" << std::flush;
                        break;
                    case CommandType::PLAY:
                        state->is_paused = false;
                        break;
                    case CommandType::PAUSE:
                        state->is_paused = true;
                        break;
                    case CommandType::SKIP:
                        state->skip_requested = true;
                        break;
                    case CommandType::PREVIOUS_TRACK: // NEW: Handle previous track request
                        if (state->history_queue.empty()) {
                            std::cout << "\n[Daemon] Cannot rewind: History queue is empty.\n" << std::flush;
                        } else {
                            state->previous_requested = true;
                            state->skip_requested = true; // Break out of active song decoding loop
                            std::cout << "\n[Daemon] Rewinding to previous track...\n" << std::flush;
                        }
                        break;
                    case CommandType::SET_VOLUME:
                        state->current_volume = cmd.int_value;
                        break;
                    case CommandType::GET_STATUS: {
                        PlayerStatusResponse res{};
                        res.current_volume     = state->current_volume;
                        res.is_paused          = state->is_paused;
                        res.user_queue_size    = static_cast<int32_t>(state->user_queue.size());
                        res.playlist_pool_size = static_cast<int32_t>(state->auto_playlist.size());
                        res.duration_seconds   = state->total_duration_seconds;

                        // ADD THESE TWO SERIALIZATION LINES
                        res.shuffle_enabled    = state->shuffle_enabled;
                        res.loop_enabled       = state->loop_enabled;

                        if (state->sample_rate > 0) {
                            res.elapsed_seconds = static_cast<int32_t>(state->accumulated_samples / state->sample_rate);
                        } else {
                            res.elapsed_seconds = 0;
                        }
                        strncpy(res.track_name, state->current_track_name.c_str(), sizeof(res.track_name) - 1);
                        res.track_name[sizeof(res.track_name) - 1] = '\0';

                        send(client_fd, &res, sizeof(PlayerStatusResponse), 0);
                        break;
                    }
            }
            pthread_mutex_unlock(&state->mutex);
        }
        close(client_fd);
    }
    close(server_fd);
    unlink(SOCKET_PATH);
}


int main(int argc, char* argv[]) {
    AudioPlayerState player_state;
    pthread_mutex_init(&player_state.mutex, nullptr);

    // Initial CLI launch parameters route cleanly into our high priority queue
    for (int i = 1; i < argc; ++i) {
        player_state.user_queue.push_back(argv[i]);
    }

    std::thread listener(socket_listener_thread, &player_state);
    listener.detach();

    std::cout << "Media Daemon Active Engine Running.\n";

    while (true) {
        std::string last_played_path = "";

        while (true) {
            std::string next_track = "";

            pthread_mutex_lock(&player_state.mutex);

            // 1. If history was requested, pop the last song off the stack
            if (player_state.previous_requested) {
                // Push the current track back to the FRONT of the user queue so we can return to it later
                if (!player_state.current_track_path.empty()) {
                    player_state.user_queue.insert(player_state.user_queue.begin(), player_state.current_track_path);
                }

                next_track = player_state.history_queue.back();
                player_state.history_queue.pop_back();
                player_state.previous_requested = false;
            }
            // 2. Otherwise, progress forward normally through your hierarchical queue structures
            else {
                // Before moving forward, log the song that just FINISHED into the history stack
                if (!player_state.current_track_path.empty()) {
                    player_state.history_queue.push_back(player_state.current_track_path);
                    // Bound check: Keep only the last 15 tracks
                    if (player_state.history_queue.size() > 15) {
                        player_state.history_queue.erase(player_state.history_queue.begin());
                    }
                }

                if (!player_state.user_queue.empty()) {
                    next_track = player_state.user_queue.front();
                    player_state.user_queue.erase(player_state.user_queue.begin());
                }
                else if (!player_state.auto_playlist.empty()) {
                    next_track = player_state.auto_playlist.front();
                    player_state.auto_playlist.erase(player_state.auto_playlist.begin());
                }
                else {
                    // Keep history alive even when idling
                    player_state.current_track_path = "";
                    pthread_mutex_unlock(&player_state.mutex);
                    usleep(100000);
                    continue;
                }
            }

            player_state.current_track_path = next_track; // Cache the active file path
            player_state.skip_requested = false;
            pthread_mutex_unlock(&player_state.mutex);

            std::cout << "\n[Playing] " << next_track << "\n" << std::flush;

            AVFormatContext* format_context = nullptr;
            if (avformat_open_input(&format_context, next_track.c_str(), nullptr, nullptr) < 0) {
                std::cerr << "Fail to open location string target: " << next_track << "\n";
                continue;
            }

            if (avformat_find_stream_info(format_context, nullptr) < 0) {
                avformat_close_input(&format_context);
                continue;
            }

            // =================================================================
            // EXTRACT METADATA TAGS (TITLE & ARTIST) FROM AV_DICTIONARY
            // =================================================================
            std::string dynamic_title = "";
            std::string dynamic_artist = "";

            // Query the dictionary case-insensitively using AV_DICT_IGNORE_SUFFIX
            AVDictionaryEntry* tag_title = av_dict_get(format_context->metadata, "title", nullptr, AV_DICT_IGNORE_SUFFIX);
            AVDictionaryEntry* tag_artist = av_dict_get(format_context->metadata, "artist", nullptr, AV_DICT_IGNORE_SUFFIX);

            if (tag_title && tag_title->value) {
                dynamic_title = tag_title->value;
            } else {
                // Fallback: If no title tag exists, extract the raw file name from the path
                size_t last_slash = next_track.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    dynamic_title = next_track.substr(last_slash + 1);
                } else {
                    dynamic_title = next_track;
                }
            }

            if (tag_artist && tag_artist->value) {
                dynamic_artist = tag_artist->value;
            } else {
                dynamic_artist = "Unknown Artist";
            }

            // Update our thread-synchronized player state name string
            pthread_mutex_lock(&player_state.mutex);
            player_state.current_track_name = dynamic_artist + " - " + dynamic_title;
            pthread_mutex_unlock(&player_state.mutex);

            std::cout << "\n[Playing Now] " << player_state.current_track_name << "\n" << std::flush;

            // Find audio stream indices and continue hardware allocation loop...
            int audio_stream_index = -1;
            AVCodecParameters* codec_params = nullptr;
            for (unsigned int i = 0; i < format_context->nb_streams; i++) {
                if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    audio_stream_index = i;
                    codec_params = format_context->streams[i]->codecpar;
                    break;
                }
            }



            if (audio_stream_index == -1) {
                avformat_close_input(&format_context);
                continue;
            }

            const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
            AVCodecContext* codec_context = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(codec_context, codec_params);
            avcodec_open2(codec_context, codec, nullptr);

            AVSampleFormat target_sample_fmt = AV_SAMPLE_FMT_S16;
            SwrContext* swr_ctx = swr_alloc();
            swr_alloc_set_opts2(&swr_ctx,
                                &codec_context->ch_layout, target_sample_fmt, codec_context->sample_rate,
                                &codec_context->ch_layout, codec_context->sample_fmt, codec_context->sample_rate,
                                0, nullptr);
            swr_init(swr_ctx);

            pthread_mutex_lock(&player_state.mutex);
            if (player_state.fifo) av_audio_fifo_free(player_state.fifo);
            player_state.fifo = av_audio_fifo_alloc(target_sample_fmt, codec_context->ch_layout.nb_channels, 1);
            pthread_mutex_unlock(&player_state.mutex);

            // =================================================================
            // Dynamically bind to the track's native sample rate
            // =================================================================
            ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
            deviceConfig.playback.format   = ma_format_s16;
            deviceConfig.playback.channels = codec_context->ch_layout.nb_channels;

            // Feed the file context parameters directly to the sound server interface
            deviceConfig.sampleRate        = codec_context->sample_rate;
            deviceConfig.dataCallback      = audio_callback;
            deviceConfig.pUserData         = &player_state;

            // Ensure our inner time tracking state always works with what was initialized
            pthread_mutex_lock(&player_state.mutex);
            player_state.sample_rate       = codec_context->sample_rate;
            player_state.accumulated_samples = 0.0;

            // Calculate total length safely from the container stream layout context
            if (format_context->duration != AV_NOPTS_VALUE) {
                player_state.total_duration_seconds = static_cast<int32_t>(format_context->duration / AV_TIME_BASE);
            } else {
                player_state.total_duration_seconds = 0;
            }
            pthread_mutex_unlock(&player_state.mutex);

            ma_device device;


            if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
                std::cerr << "Hardware binding exception\n";
                continue;
            }
            ma_device_start(&device);

            AVPacket* packet = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();

            while (av_read_frame(format_context, packet) >= 0) {
                pthread_mutex_lock(&player_state.mutex);
                if (player_state.skip_requested || player_state.quit_requested) {
                    pthread_mutex_unlock(&player_state.mutex);
                    break;
                }
                pthread_mutex_unlock(&player_state.mutex);

                if (packet->stream_index == audio_stream_index) {
                    if (avcodec_send_packet(codec_context, packet) >= 0) {
                        while (avcodec_receive_frame(codec_context, frame) >= 0) {
                            uint8_t* converted_output_buffer = nullptr;
                            int out_samples = frame->nb_samples;
                            av_samples_alloc(&converted_output_buffer, nullptr, codec_context->ch_layout.nb_channels, out_samples, target_sample_fmt, 0);

                            swr_convert(swr_ctx, &converted_output_buffer, out_samples, (const uint8_t**)frame->data, frame->nb_samples);

                            pthread_mutex_lock(&player_state.mutex);
                            av_audio_fifo_write(player_state.fifo, (void**)&converted_output_buffer, out_samples);
                            pthread_mutex_unlock(&player_state.mutex);

                            av_freep(&converted_output_buffer);

                            while (true) {
                                pthread_mutex_lock(&player_state.mutex);
                                int size = av_audio_fifo_size(player_state.fifo);
                                bool skip = player_state.skip_requested;
                                pthread_mutex_unlock(&player_state.mutex);

                                if (size <= (int)codec_context->sample_rate * 2 || skip) break;
                                usleep(10000);
                            }
                        }
                    }
                }
                av_packet_unref(packet);
            }

            // Complete track sequence drainage calculations
            int static_count = 0;
            int last_sample_count = -1;

            while (true) {
                pthread_mutex_lock(&player_state.mutex);
                int remaining_samples = av_audio_fifo_size(player_state.fifo);
                bool skip = player_state.skip_requested;
                pthread_mutex_unlock(&player_state.mutex);

                // Break instantly if a manual skip was sent
                if (skip) break;

                // If the buffer is completely empty, we are safe to advance
                if (remaining_samples <= 0) break;

                // Track if the sample count has stalled (meaning miniaudio stopped consuming it)
                if (remaining_samples == last_sample_count) {
                    static_count++;

                } else {
                    static_count = 0; // Reset if data is still actively draining
                    last_sample_count = remaining_samples;
                }

                // If the buffer has been stagnant for 100ms, the song is naturally over
                if (static_count >= 5) {
                    std::cout << "[Daemon] Buffer drain complete (tail silence reached).\n";
                    break;
                }
                usleep(20000); // 20ms check intervals
            }

            // Securely stop and tear down the hardware channels
            ma_device_stop(&device);
            ma_device_uninit(&device);

            av_frame_free(&frame);
            av_packet_free(&packet);
            swr_free(&swr_ctx);
            avcodec_free_context(&codec_context);
            avformat_close_input(&format_context);

            std::cout << "[Daemon] Track wrapped up cleanly. Loading next in queue...\n" << std::flush;

        }

        pthread_mutex_destroy(&player_state.mutex);
        return 0;
    }
}
