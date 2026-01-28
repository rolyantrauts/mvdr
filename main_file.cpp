#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <fftw3.h>
#include <cstring>
#include "mvdr_neon.h"

// --- Config matches your live settings ---
#define SAMPLE_RATE 16000
#define FRAME_SIZE 512
#define HOP_SIZE 256
#define NUM_BINS 257
#define PI 3.14159265358979323846f

// --- WAV Header Struct ---
struct WavHeader {
    char riff[4];           // "RIFF"
    uint32_t overall_size;  // File size - 8
    char wave[4];           // "WAVE"
    char fmt_chunk_marker[4]; // "fmt "
    uint32_t length_of_fmt; // 16
    uint16_t format_type;   // 1 = PCM
    uint16_t channels;      // 2 for Stereo
    uint32_t sample_rate;   // 16000
    uint32_t byterate;      // SampleRate * NumChannels * BitsPerSample/8
    uint16_t block_align;   // NumChannels * BitsPerSample/8
    uint16_t bits_per_sample; // 16
    char data_chunk_header[4]; // "data"
    uint32_t data_size;     // Size of data section
};

// --- Helpers ---
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

int main(int argc, char** argv) {
    std::string in_file, out_file;
    float angle = 90.0f;
    float dist = 0.058f;

    // Arg Parse
    for(int i=1; i<argc; ++i) {
        std::string arg = argv[i];
        if(arg == "-i") in_file = argv[++i];
        else if(arg == "-o") out_file = argv[++i];
        else if(arg == "-a") angle = std::stof(argv[++i]);
        else if(arg == "-d") dist = std::stof(argv[++i]);
    }

    if(in_file.empty() || out_file.empty()) {
        std::cerr << "Usage: ./mvdr_file -i input.wav -o output.wav -a 90 -d 0.058\n";
        return 1;
    }

    // --- Read WAV ---
    std::ifstream f_in(in_file, std::ios::binary);
    if(!f_in.is_open()) { std::cerr << "Cannot open " << in_file << "\n"; return 1; }
    
    WavHeader head;
    f_in.read((char*)&head, sizeof(WavHeader));
    
    // Basic validation
    if(head.channels != 2) { std::cerr << "Input must be Stereo (2 ch)\n"; return 1; }
    if(head.sample_rate != 16000) { std::cerr << "Input must be 16000Hz\n"; return 1; }

    // Read Data
    std::vector<int16_t> raw_data(head.data_size / 2);
    f_in.read((char*)raw_data.data(), head.data_size);
    f_in.close();

    // Prepare float buffers
    int num_samples = raw_data.size() / 2; // Stereo samples
    std::vector<float> ch1(num_samples), ch2(num_samples);
    for(int i=0; i<num_samples; ++i) {
        ch1[i] = raw_data[2*i] / 32768.0f;
        ch2[i] = raw_data[2*i+1] / 32768.0f;
    }

    // --- Setup MVDR ---
    fftwf_complex *in_fft_ch1 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    fftwf_complex *in_fft_ch2 = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    fftwf_complex *out_fft    = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * NUM_BINS);
    float* sv1 = (float*) aligned_alloc(16, NUM_BINS * 2 * sizeof(float) + 64);
    float* sv2 = (float*) aligned_alloc(16, NUM_BINS * 2 * sizeof(float) + 64);
    update_steering_vector(sv1, sv2, angle, dist);

    float* time_buf_ch1 = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);
    float* time_buf_ch2 = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);
    float* time_out_buf = (float*) fftwf_malloc(sizeof(float) * FRAME_SIZE);
    
    fftwf_plan p1 = fftwf_plan_dft_r2c_1d(FRAME_SIZE, time_buf_ch1, in_fft_ch1, FFTW_ESTIMATE);
    fftwf_plan p2 = fftwf_plan_dft_r2c_1d(FRAME_SIZE, time_buf_ch2, in_fft_ch2, FFTW_ESTIMATE);
    fftwf_plan p_out = fftwf_plan_dft_c2r_1d(FRAME_SIZE, out_fft, time_out_buf, FFTW_ESTIMATE);

    MvdrState mvdr;
    mvdr_init(&mvdr);

    std::vector<float> out_pcm_float(num_samples, 0.0f);
    std::vector<float> out_overlap(FRAME_SIZE, 0.0f);
    std::vector<float> window(FRAME_SIZE);
    create_hanning_window(window.data(), FRAME_SIZE);
    float scale = 1.0f / FRAME_SIZE;

    // --- Processing Loop ---
    int num_frames = (num_samples - FRAME_SIZE) / HOP_SIZE;
    
    for(int n=0; n<num_frames; ++n) {
        int idx = n * HOP_SIZE;
        
        // Window
        for(int i=0; i<FRAME_SIZE; ++i) {
            time_buf_ch1[i] = ch1[idx+i] * window[i];
            time_buf_ch2[i] = ch2[idx+i] * window[i];
        }

        fftwf_execute(p1);
        fftwf_execute(p2);
        
        // MVDR Math
        process_mvdr_neon(&mvdr, (float*)in_fft_ch1, (float*)in_fft_ch2, sv1, sv2, (float*)out_fft, NUM_BINS);
        
        fftwf_execute(p_out);

        // Overlap Add
        for(int i=0; i<FRAME_SIZE; ++i) {
            // Write to overlap buffer
            out_overlap[i] += time_out_buf[i] * scale;
        }

        // Output valid part (HOP_SIZE)
        for(int i=0; i<HOP_SIZE; ++i) {
             out_pcm_float[idx+i] = out_overlap[i];
        }

        // Shift Overlap
        std::copy(out_overlap.begin()+HOP_SIZE, out_overlap.end(), out_overlap.begin());
        std::fill(out_overlap.begin()+(FRAME_SIZE-HOP_SIZE), out_overlap.end(), 0.0f);
    }

    // --- Write WAV ---
    std::vector<int16_t> out_raw;
    for(float s : out_pcm_float) {
        if(s > 1.0f) s = 1.0f; 
        if(s < -1.0f) s = -1.0f;
        out_raw.push_back((int16_t)(s * 32767.0f));
    }

    std::ofstream f_out(out_file, std::ios::binary);
    
    // Create Mono Header
    WavHeader out_h = head;
    out_h.channels = 1;
    out_h.block_align = 2; // 16-bit mono
    out_h.byterate = 16000 * 2;
    out_h.data_size = out_raw.size() * 2;
    out_h.overall_size = out_h.data_size + 36;

    f_out.write((char*)&out_h, sizeof(WavHeader));
    f_out.write((char*)out_raw.data(), out_raw.size()*2);
    f_out.close();

    std::cout << "Processed " << num_frames << " frames. Saved to " << out_file << "\n";
    return 0;
}
