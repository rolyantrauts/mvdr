#include <iostream>
#include <vector>
#include <cmath>
#include <string>
#include <alsa/asoundlib.h>
#include <fftw3.h>
#include <iomanip>
#include <csignal>
#include <unistd.h> 
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <fcntl.h>      
#include "mvdr_neon.h"

// --- Configuration ---
#define SAMPLE_RATE 16000
#define CHANNELS_IN 2
#define CHANNELS_OUT 1
#define FRAME_SIZE 512
#define HOP_SIZE 256
#define NUM_BINS 257
#define PI 3.14159265358979323846f
#define UDP_PORT 5555

volatile bool running = true;

// --- Helper: UDP Listener ---
int setup_udp_socket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return -1;

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY; 
    servaddr.sin_port = htons(UDP_PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) return -1;

    // Set Non-Blocking
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    return sockfd;
}

// --- Helper: DOA History Buffer (Weighted Average) ---
struct FrameStat {
    float rms;
    float angle;
};

class DoaHistory {
    std::vector<FrameStat> buffer;
    size_t head = 0;
    size_t size;
public:
    DoaHistory(size_t n_frames) : size(n_frames) {
        if (size < 1) size = 1;
        buffer.resize(size, {0.0f, 90.0f});
    }

    void push(float rms, float angle) {
        buffer[head] = {rms, angle};
        head = (head + 1) % size;
    }

    // Calculates the Weighted Average Angle based on Energy (RMS^2)
    // This ignores quiet frames and focuses on where the main speech energy came from.
    float get_best_angle() {
        float sum_sin = 0.0f;
        float sum_cos = 0.0f;
        float total_weight = 0.0f;

        for (const auto& frame : buffer) {
            // Use squared RMS as weight to heavily penalize silence/noise
            float weight = frame.rms * frame.rms;
            
            // Ignore very quiet frames to prevent divide-by-zero or noise bias
            if (weight < 1e-8f) continue;

            // Convert to radians for vector averaging
            float rad = frame.angle * PI / 180.0f;
            sum_sin += sinf(rad) * weight;
            sum_cos += cosf(rad) * weight;
            total_weight += weight;
        }

        // If history is empty or silent, default to center
        if (total_weight < 1e-8f) return 90.0f;

        float avg_rad = atan2f(sum_sin, sum_cos);
        float avg_deg = avg_rad * 180.0f / PI;

        // Clamp to valid range (0-180)
        if (avg_deg < 0.0f) avg_deg = 0.0f;
        if (avg_deg > 180.0f) avg_deg = 180.0f;

        return avg_deg;
    }
};

void signal_handler(int sig) { running = false; }

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " [OPTIONS] | aplay ...\n"
              << "  -i [dev]   Input ALSA device (default: plughw:1,0)\n"
              << "  -d [m]     Mic Spacing (default: 0.058)\n"
              << "  -g [gain]  Output Gain (default: 1.0)\n"
              << "  -f [num]   DOA History Frames (default: 100)\n"
              << "  -p         Pass-through Mode (Debug)\n"
              << "  -v         Verbose DOA (Print angles to stderr)\n";
}

// --- Utils ---
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
    for (int i = 0; i < size; ++i) 
        window[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (size - 1)));
}

snd_pcm_t* open_capture(const char* name, int channels) {
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0) < 0) return nullptr;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm, hw_params);
    snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw_params, channels);
    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, 0);
    unsigned int buffer_time = 500000; // 0.5s buffer
    snd_pcm_hw_params_set_buffer_time_near(pcm, hw_params, &buffer_time, 0);
    if (snd_pcm_hw_params(pcm, hw_params) < 0) return nullptr;
    if (snd_pcm_prepare(pcm) < 0) return nullptr;
    return pcm;
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);

    std::string input_dev = "plughw:1,0";
    float target_angle = 90.0f;
    float current_doa = 90.0f;
    float mic_spacing = 0.058f;
    float output_gain = 1.0f;
    int history_frames = 100; // Default ~1.6 seconds
    bool passthrough = false;
    bool verbose_doa = false;
    bool auto_steering = true; 

    // --- Argument Parsing ---
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) input_dev = argv[++i];
        else if (arg == "-d" && i + 1 < argc) mic_spacing = std::stof(argv[++i]);
        else if (arg == "-g" && i + 1 < argc) output_gain = std::stof(argv[++i]);
        else if (arg == "-f" && i + 1 < argc) history_frames = std::stoi(argv[++i]);
        else if (arg == "-p") passthrough = true;
        else if (arg == "-v") verbose_doa = true;
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
    }

    std::cerr << "MVDR Engine Started (UDP: " << UDP_PORT << ", History: " << history_frames << " frames)\n";

    snd_pcm_t *capture = open_capture(input_dev.c_str(), CHANNELS_IN);
    if (!capture) { std::cerr << "Failed to open capture device\n"; return 1; }

    int udp_sock = setup_udp_socket();
    char udp_buf[1024];
    
    // Initialize History Buffer with parsed size
    DoaHistory history(history_frames); 

    // FFTW & Buffers Setup
    fftwf_complex *in_fft_ch1 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    fftwf_complex *in_fft_ch2 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    fftwf_complex *out_fft    = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    
    // Scratch buffers for DOA
    fftwf_complex *doa_fft_in = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    float* doa_time_out       = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);

    float* sv1 = (float*) aligned_alloc(16, NUM_BINS * 2 * sizeof(float) + 64); 
    float* sv2 = (float*) aligned_alloc(16, NUM_BINS * 2 * sizeof(float) + 64);
    update_steering_vector(sv1, sv2, target_angle, mic_spacing);
    
    float* time_buf_ch1 = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);
    float* time_buf_ch2 = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);
    float* time_out_buf = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);

    fftwf_plan p1 = fftwf_plan_dft_r2c_1d(FRAME_SIZE, time_buf_ch1, in_fft_ch1, FFTW_ESTIMATE);
    fftwf_plan p2 = fftwf_plan_dft_r2c_1d(FRAME_SIZE, time_buf_ch2, in_fft_ch2, FFTW_ESTIMATE);
    fftwf_plan p_out = fftwf_plan_dft_c2r_1d(FRAME_SIZE, out_fft, time_out_buf, FFTW_ESTIMATE);
    fftwf_plan p_doa = fftwf_plan_dft_c2r_1d(FRAME_SIZE, doa_fft_in, doa_time_out, FFTW_ESTIMATE);

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

        // Process Audio to Float
        float rms_sq = 0.0f;
        float dc_ch1 = 0.0f, dc_ch2 = 0.0f;
        for(int i=0; i<HOP_SIZE; ++i) {
            float s1 = (float)in_raw[2*i] / 32768.0f;
            float s2 = (float)in_raw[2*i+1] / 32768.0f;
            dc_ch1 += s1; dc_ch2 += s2;
            in_float[2*i] = s1; in_float[2*i+1] = s2;
            rms_sq += s1*s1 + s2*s2; 
        }
        float frame_rms = sqrt(rms_sq / (HOP_SIZE * 2));
        dc_ch1 /= HOP_SIZE; dc_ch2 /= HOP_SIZE;
        for(int i=0; i<HOP_SIZE; ++i) {
            in_float[2*i] -= dc_ch1; in_float[2*i+1] -= dc_ch2;
        }

        // Pass-through Mode: Skip Math, Just Output
        if (passthrough) {
            for(int i=0; i<HOP_SIZE; ++i) {
                float val = in_float[2*i] * output_gain; // Just output Left Channel
                if(val > 1.0f) val = 1.0f; else if(val < -1.0f) val = -1.0f;
                out_raw[i] = (int16_t)(val * 32767.0f);
            }
        } 
        else {
            // Full Processing
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
            
            fftwf_execute(p1); 
            fftwf_execute(p2);

            // DOA Calc (Always Run to keep History Fresh)
            float estimated_angle = calculate_doa_gcc_phat(in_fft_ch1, in_fft_ch2, 
                                                           doa_fft_in, doa_time_out, p_doa, 
                                                           mic_spacing, FRAME_SIZE, SAMPLE_RATE);
            
            // Update History
            if (frame_rms > 0.01f) {
                 current_doa = 0.9f * current_doa + 0.1f * estimated_angle;
            }
            history.push(frame_rms, estimated_angle);

            // UDP Control
            int n = recvfrom(udp_sock, udp_buf, 1024, 0, NULL, NULL);
            if (n > 0) {
                udp_buf[n] = '\0';
                std::string cmd(udp_buf);
                if (cmd.find("LOCK") == 0) {
                    float best_angle = history.get_best_angle();
                    if(verbose_doa) std::cerr << "CMD: LOCK " << (int)best_angle << "\n";
                    auto_steering = false;
                    target_angle = best_angle;
                    update_steering_vector(sv1, sv2, target_angle, mic_spacing);
                }
                else if (cmd.find("RESET") == 0) {
                    if(verbose_doa) std::cerr << "CMD: RESET\n";
                    auto_steering = true;
                }
                else if (cmd.find("SET") == 0) {
                    try {
                        float new_ang = std::stof(cmd.substr(4));
                        if(verbose_doa) std::cerr << "CMD: SET " << new_ang << "\n";
                        auto_steering = false;
                        target_angle = new_ang;
                        update_steering_vector(sv1, sv2, target_angle, mic_spacing);
                    } catch (...) {}
                }
            }

            // Auto Steer Logic
            if (auto_steering && frame_rms > 0.05f) {
                if (std::abs(current_doa - target_angle) > 5.0f) {
                    target_angle = 0.95f * target_angle + 0.05f * current_doa;
                    update_steering_vector(sv1, sv2, target_angle, mic_spacing);
                }
            }

            // MVDR Beamforming
            process_mvdr_neon(&mvdr_state, (float*)in_fft_ch1, (float*)in_fft_ch2, sv1, sv2, (float*)out_fft, NUM_BINS);
            fftwf_execute(p_out);

            // Overlap Add Output
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

        // Output Audio to STDOUT
        write(STDOUT_FILENO, out_raw.data(), HOP_SIZE * sizeof(int16_t));

        // Verbose DOA Logging (Conditional)
        if (verbose_doa && auto_steering && block_cnt++ % 20 == 0) {
            std::cerr << "DOA: " << (int)current_doa << "\n";
        }
    }
    return 0;
}
