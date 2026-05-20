#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
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
#include "protocol.h" // Pull in our shared socket structures

// This structure holds our shared global variables accessible across our threads
struct AudioPlayerState {
    AVAudioFifo* fifo = nullptr;
    pthread_mutex_t mutex;

    // Shared state controls
    std::vector<std::string> song_queue;
    int current_volume = 80;       // Volume level scaled from 0-100
    bool is_paused = false;
    bool skip_requested = false;
    bool quit_requested = false;
    std::string current_track_name = "None";
};

// =================================================================
// 1. MINIAUDIO HARDWARE CALLBACK WITH SOFTWARE VOLUME MULTIPLIER
// =================================================================
void audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioPlayerState* state = (AudioPlayerState*)pDevice->pUserData;
    if (!state || !state->fifo) return;

    pthread_mutex_lock(&state->mutex);

    // If the user paused the player via player-ctl, output silence to the speakers
    if (state->is_paused) {
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    // Check if the FIFO ring buffer has enough samples to feed the audio card
    if (av_audio_fifo_size(state->fifo) < (int)frameCount) {
        // Output silence briefly if the decoder loop falls slightly behind
        memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        pthread_mutex_unlock(&state->mutex);
        return;
    }

    // Pull raw Interleaved PCM data straight out of our FIFO buffer
    uint8_t* output_buffer = (uint8_t*)pOutput;
    av_audio_fifo_read(state->fifo, (void**)&output_buffer, frameCount);

    // Grab the active volume level to apply real-time math scaling
    float volume_factor = state->current_volume / 100.0f;
    pthread_mutex_unlock(&state->mutex);

    // Scale our individual audio waves mathematically
    int16_t* samples = (int16_t*)pOutput;
    int total_samples = frameCount * pDevice->playback.channels;
    for (int i = 0; i < total_samples; ++i) {
        samples[i] = (int16_t)(samples[i] * volume_factor);
    }
}

// =================================================================
// 2. UNIX DOMAIN SOCKET LISTENER BACKGROUND THREAD
// =================================================================
void socket_listener_thread(AudioPlayerState* state) {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return;

    unlink(SOCKET_PATH); // Wipe old socket node artifacts from previous runs

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return;
    }

    listen(server_fd, 10);

    while (true) {
        // Check if main thread asked to clean down the program
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

            // Process the incoming command block safely inside our mutex barrier
            switch (cmd.type) {
                case CommandType::ADD_TO_QUEUE:
                    state->song_queue.push_back(cmd.path_value);
                    std::cout << "\n[Daemon] Added track to queue: " << cmd.path_value << "\n" << std::flush;
                    break;
                case CommandType::PLAY:
                    state->is_paused = false;
                    std::cout << "\n[Daemon] Playback Resumed.\n" << std::flush;
                    break;
                case CommandType::PAUSE:
                    state->is_paused = true;
                    std::cout << "\n[Daemon] Playback Paused.\n" << std::flush;
                    break;
                case CommandType::SKIP:
                    state->skip_requested = true;
                    std::cout << "\n[Daemon] Skipping current track...\n" << std::flush;
                    break;
                case CommandType::SET_VOLUME:
                    state->current_volume = cmd.int_value;
                    std::cout << "\n[Daemon] Volume updated to: " << state->current_volume << "%\n" << std::flush;
                    break;
                case CommandType::GET_STATUS:
                    // Print status directly on our daemon logger stdout window
                    std::cout << "\n--- Current Player Status Query ---\n"
                    << "Active Track: " << state->current_track_name << "\n"
                    << "Volume: " << state->current_volume << "%\n"
                    << "State: " << (state->is_paused ? "PAUSED" : "PLAYING") << "\n"
                    << "Tracks in Queue: " << state->song_queue.size() << "\n"
                    << "-----------------------------------\n" << std::flush;
                    break;
            }
            pthread_mutex_unlock(&state->mutex);
        }
        close(client_fd);
    }
    close(server_fd);
    unlink(SOCKET_PATH);
}

// =================================================================
// 3. MAIN AUDIO DECODING AND PLAYBACK STREAM ENGINE
// =================================================================
int main(int argc, char* argv[]) {
    AudioPlayerState player_state;
    pthread_mutex_init(&player_state.mutex, nullptr);

    // Seed initial queue if files are passed directly to our binary startup execution call
    for (int i = 1; i < argc; ++i) {
        player_state.song_queue.push_back(argv[i]);
    }

    // Initialize the background socket listener execution thread
    std::thread listener(socket_listener_thread, &player_state);
    listener.detach(); // Allow the listener thread to cycle freely in the background

    std::cout << "Media Daemon Active. Listening on: " << SOCKET_PATH << "\n";

    // Continuous main application tracking loop
    while (true) {
        std::string next_track = "";

        pthread_mutex_lock(&player_state.mutex);
        if (player_state.song_queue.empty()) {
            pthread_mutex_unlock(&player_state.mutex);
            // Throttle main execution wheel while waiting for player-ctl to populate the queue
            usleep(100000); // 100ms idle window
            continue;
        }

        // Extract the front item from our queue array
        next_track = player_state.song_queue.front();
        player_state.song_queue.erase(player_state.song_queue.begin());
        player_state.current_track_name = next_track;
        player_state.skip_requested = false;
        pthread_mutex_unlock(&player_state.mutex);

        std::cout << "\nNow Running: " << next_track << "\n" << std::flush;

        // Standard FFmpeg file handling
        AVFormatContext* format_context = nullptr;
        if (avformat_open_input(&format_context, next_track.c_str(), nullptr, nullptr) < 0) {
            std::cerr << "Could not open file: " << next_track << "\n";
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

        // Dynamically initialize or flush the active storage ring buffer
        pthread_mutex_lock(&player_state.mutex);
        if (player_state.fifo) av_audio_fifo_free(player_state.fifo);
            player_state.fifo = av_audio_fifo_alloc(target_sample_fmt, codec_context->ch_layout.nb_channels, 1);
            pthread_mutex_unlock(&player_state.mutex);

            // Configure miniaudio link targets
            ma_device_config deviceConfig  = ma_device_config_init(ma_device_type_playback);
            deviceConfig.playback.format   = ma_format_s16;
            deviceConfig.playback.channels = codec_context->ch_layout.nb_channels;
            deviceConfig.sampleRate        = codec_context->sample_rate;
            deviceConfig.dataCallback      = audio_callback;
            deviceConfig.pUserData         = &player_state;

            ma_device device;

        if (ma_device_init(NULL, &deviceConfig, &device) == MA_SUCCESS) {
            ma_device_start(&device);
        } else {
            std::cerr << "Could not bind miniaudio channel context\n";
        }

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        // Core extraction loop for the active track
        while (av_read_frame(format_context, packet) >= 0) {
            pthread_mutex_lock(&player_state.mutex);
            // Check if the user initiated a skip or program shutdown
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

                        // Throttle data generation loop if buffer gets full
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

        // Wait until the audio card finishes draining the remaining track fragments out of the FIFO queue
        while (true) {
            pthread_mutex_lock(&player_state.mutex);
            int remaining_samples = av_audio_fifo_size(player_state.fifo);
            bool skip = player_state.skip_requested;
            pthread_mutex_unlock(&player_state.mutex);

            if (remaining_samples <= 0 || skip) break;
            usleep(20000);
        }

        // 1. Explicitly stop and close the hardware playback interface before cleanup
        ma_device_stop(&device);
        ma_device_uninit(&device);

        // 2. Clear out the rest of your FFmpeg pointers
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format_context);

        std::cout << "Track execution block finished. Advancing...\n" << std::flush;
    }

    pthread_mutex_destroy(&player_state.mutex);
    return 0;
}
