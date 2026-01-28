#include <iostream>
#include <vector>
#include <cmath>
#include <string>
#include <alsa/asoundlib.h>
#include <fftw3.h>
#include <iomanip>
#include <csignal>
#include <unistd.h> // For write()
#include "mvdr_neon.h"

// --- Configuration ---
#define SAMPLE_RATE 16000
#define CHANNELS_IN 2
#define CHANNELS_OUT 1
#define FRAME_SIZE 512
#define HOP_SIZE 256
#define NUM_BINS 257
#define PI 3.14159265358979323846f

volatile bool running = true;

void signal_handler(int sig) {
    running = false;
}

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " [OPTIONS] | aplay ...\n"
              << "  -i [dev]   Input ALSA device (default: plughw:1,0)\n"
              << "  -a [deg]   Steering Angle (default: 90.0)\n"
              << "  -d [m]     Mic Spacing (default: 0.058)\n"
              << "  -g [gain]  Gain (default: 1.0)\n"
              << "  -p         Pass-through Mode\n";
}

void update_steering_vector(float* sv1, float* sv2, float angle_deg, float d_meters) {
    float theta = angle_deg * PI / 180.0f;
    float tau = (d_meters * cos(theta)) / 343.0f; 
    for (int k = 0; k < NUM_BINS; ++k) {
        float freq = (float)k * SAMPLE_RATE / FRAME_SIZE;
        float omega_tau = 2.0f * PI * freq * tau;
        sv1[2*k] = 1.0f; sv1[2*k+1] = 0.0f;
        sv2[2*k] = cosf(-omega_tau); sv2[2*k+1] = sinf(-omega_tau);
    }
}

void create_hanning_window(float* window, int size) {
    for (int i = 0; i < size; ++i) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (size - 1)));
    }
}

// ALSA Opener (Input Only)
snd_pcm_t* open_capture(const char* name, int channels) {
    snd_pcm_t *pcm;
    int err;
    if ((err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        std::cerr << "Error opening input: " << snd_strerror(err) << std::endl;
        return nullptr;
    }
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm, hw_params);
    snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw_params, channels);
    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, 0);
    
    // Deep buffer for Input safety
    unsigned int buffer_time = 500000; 
    snd_pcm_hw_params_set_buffer_time_near(pcm, hw_params, &buffer_time, 0);

    if (snd_pcm_hw_params(pcm, hw_params) < 0) return nullptr;
    if (snd_pcm_prepare(pcm) < 0) return nullptr;
    return pcm;
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);

    // Note: We use std::cerr for logs because std::cout is now AUDIO DATA
    std::string input_dev = "plughw:1,0";
    float steering_angle = 90.0f;
    float mic_spacing = 0.058f;
    float output_gain = 1.0f;
    bool passthrough = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) input_dev = argv[++i];
        else if (arg == "-a" && i + 1 < argc) steering_angle = std::stof(argv[++i]);
        else if (arg == "-d" && i + 1 < argc) mic_spacing = std::stof(argv[++i]);
        else if (arg == "-g" && i + 1 < argc) output_gain = std::stof(argv[++i]);
        else if (arg == "-p") passthrough = true;
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
    }

    std::cerr << "MVDR Streamer (Pipe Mode)" << std::endl;

    snd_pcm_t *capture = open_capture(input_dev.c_str(), CHANNELS_IN);
    if (!capture) return 1;

    // FFTW Setup
    fftwf_complex *in_fft_ch1 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    fftwf_complex *in_fft_ch2 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    fftwf_complex *out_fft    = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    float* sv1 = (float*) aligned_alloc(16, NUM_BINS * 2 * sizeof(float) + 64); 
    float* sv2 = (float*) aligned_alloc(16, NUM_BINS * 2 * sizeof(float) + 64);
    update_steering_vector(sv1, sv2, steering_angle, mic_spacing);
    
    float* time_buf_ch1 = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);
    float* time_buf_ch2 = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);
    float* time_out_buf = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);

    fftwf_plan p1 = fftwf_plan_dft_r2c_1d(FRAME_SIZE, time_buf_ch1, in_fft_ch1, FFTW_ESTIMATE);
    fftwf_plan p2 = fftwf_plan_dft_r2c_1d(FRAME_SIZE, time_buf_ch2, in_fft_ch2, FFTW_ESTIMATE);
    fftwf_plan p_out = fftwf_plan_dft_c2r_1d(FRAME_SIZE, out_fft, time_out_buf, FFTW_ESTIMATE);

    MvdrState mvdr_state;
    mvdr_init(&mvdr_state);

    std::vector<int16_t> in_raw(HOP_SIZE * CHANNELS_IN);
    std::vector<int16_t> out_raw(HOP_SIZE * CHANNELS_OUT);
    std::vector<float> in_float(HOP_SIZE * CHANNELS_IN);
    std::vector<float> ring_1(FRAME_SIZE, 0.0f);
    std::vector<float> ring_2(FRAME_SIZE, 0.0f);
    std::vector<float> out_overlap(FRAME_SIZE, 0.0f);
    std::vector<float> window(FRAME_SIZE);
    create_hanning_window(window.data(), FRAME_SIZE);
    float scale = 1.0f / FRAME_SIZE; 

    long block_cnt = 0;
    while (running) {
        long err = snd_pcm_readi(capture, in_raw.data(), HOP_SIZE);
        if (err < 0) { if (err == -EPIPE) { snd_pcm_prepare(capture); continue; } break; }

        // DC Block
        float dc_ch1 = 0.0f, dc_ch2 = 0.0f;
        for(int i=0; i<HOP_SIZE; ++i) {
            float s1 = (float)in_raw[2*i] / 32768.0f;
            float s2 = (float)in_raw[2*i+1] / 32768.0f;
            dc_ch1 += s1; dc_ch2 += s2;
            in_float[2*i] = s1; in_float[2*i+1] = s2;
        }
        dc_ch1 /= HOP_SIZE; dc_ch2 /= HOP_SIZE;
        for(int i=0; i<HOP_SIZE; ++i) {
            in_float[2*i] -= dc_ch1; in_float[2*i+1] -= dc_ch2;
        }

        if (passthrough) {
            for(int i=0; i<HOP_SIZE; ++i) {
                float val = in_float[2*i] * output_gain;
                if(val > 1.0f) val = 1.0f; else if(val < -1.0f) val = -1.0f;
                out_raw[i] = (int16_t)(val * 32767.0f);
            }
        } else {
            // MVDR Process
            std::copy(ring_1.begin()+HOP_SIZE, ring_1.end(), ring_1.begin());
            std::copy(ring_2.begin()+HOP_SIZE, ring_2.end(), ring_2.begin());
            for(int i=0; i<HOP_SIZE; ++i) {
                ring_1[FRAME_SIZE-HOP_SIZE+i] = in_float[2*i];
                ring_2[FRAME_SIZE-HOP_SIZE+i] = in_float[2*i+1];
            }
            for(int i=0; i<FRAME_SIZE; ++i) {
                time_buf_ch1[i] = ring_1[i] * window[i];
                time_buf_ch2[i] = ring_2[i] * window[i];
            }
            fftwf_execute(p1); fftwf_execute(p2);
            process_mvdr_neon(&mvdr_state, (float*)in_fft_ch1, (float*)in_fft_ch2, sv1, sv2, (float*)out_fft, NUM_BINS);
            fftwf_execute(p_out);

            for(int i=0; i<FRAME_SIZE; ++i) {
                out_overlap[i] += time_out_buf[i] * scale;
                if(i < HOP_SIZE) {
                    float val = out_overlap[i] * output_gain;
                    if(val > 1.0f) val = 1.0f; else if(val < -1.0f) val = -1.0f;
                    out_raw[i] = (int16_t)(val * 32767.0f);
                }
            }
            std::copy(out_overlap.begin()+HOP_SIZE, out_overlap.end(), out_overlap.begin());
            std::fill(out_overlap.begin()+(FRAME_SIZE-HOP_SIZE), out_overlap.end(), 0.0f);
        }

        // --- THE MAGIC: WRITE TO STDOUT ---
        // We use write() to dump raw bytes to File Descriptor 1 (stdout)
        // This blocks only if the pipe is full, but aplay reads fast.
        write(STDOUT_FILENO, out_raw.data(), HOP_SIZE * sizeof(int16_t));

        // Optional Log (to stderr so it doesn't pollute audio)
        if (block_cnt++ % 62 == 0) {
            float rms = 0.0f;
            for(int i=0; i<HOP_SIZE; ++i) rms += (in_float[2*i]*in_float[2*i]);
            rms = sqrt(rms / HOP_SIZE);
            std::cerr << "\rRMS: " << std::fixed << std::setprecision(5) << rms << "   " << std::flush;
        }
    }
    return 0;
}
