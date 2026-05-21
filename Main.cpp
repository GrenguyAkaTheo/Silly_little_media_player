#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

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

    // TWO SEPARATE TRACK CONTAINERS
    std::vector<std::string> user_queue;     // Populated by --add
    std::vector<std::string> auto_playlist;   // Populated by --playlist (.m3u parsing)

    int current_volume = 80;
    bool is_paused = false;
    bool skip_requested = false;
    bool quit_requested = false;
    std::string current_track_name = "None";
};

void audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioPlayerState* state = (AudioPlayerState*)pDevice->pUserData;
    if (!state || !state->fifo) return;

    pthread_mutex_lock(&state->mutex);

    if (state->is_paused) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    if (av_audio_fifo_size(state->fifo) < (int)frameCount) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    uint8_t* output_buffer = (uint8_t*)pOutput;
    av_audio_fifo_read(state->fifo, (void**)&output_buffer, frameCount);

    float volume_factor = state->current_volume / 100.0f;
    pthread_mutex_unlock(&state->mutex);

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
                    state->auto_playlist.push_back(cmd.path_value);
                    break;
                case CommandType::CLEAR_PLAYLIST:
                    state->auto_playlist.clear();
                    std::cout << "\n[Playlist] Context cleared completely.\n" << std::flush;
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
                case CommandType::SET_VOLUME:
                    state->current_volume = cmd.int_value;
                    break;
                case CommandType::GET_STATUS:
                    std::cout << "\n--- Media Engine Monitor ---\n"
                    << "Playing Now: " << state->current_track_name << "\n"
                    << "Volume State: " << state->current_volume << "%\n"
                    << "Playback Context: " << (state->is_paused ? "PAUSED" : "PLAYING") << "\n"
                    << "User Queue Size: " << state->user_queue.size() << " tracks\n"
                    << "Playlist Pool Size: " << state->auto_playlist.size() << " tracks\n"
                    << "----------------------------\n" << std::flush;
                    break;
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
        std::string next_track = "";

        pthread_mutex_lock(&player_state.mutex);

        // HIERARCHICAL QUEUE RESOLUTION LOGIC
        if (!player_state.user_queue.empty()) {
            next_track = player_state.user_queue.front();
            player_state.user_queue.erase(player_state.user_queue.begin());
        }
        else if (!player_state.auto_playlist.empty()) {
            next_track = player_state.auto_playlist.front();
            player_state.auto_playlist.erase(player_state.auto_playlist.begin());
        }
        else {
            pthread_mutex_unlock(&player_state.mutex);
            usleep(100000); // Rest if all queues sit empty
            continue;
        }

        player_state.current_track_name = next_track;
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

        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format   = ma_format_s16;
        deviceConfig.playback.channels = codec_context->ch_layout.nb_channels;
        deviceConfig.sampleRate        = codec_context->sample_rate;
        deviceConfig.dataCallback      = audio_callback;
        deviceConfig.pUserData         = &player_state;

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
