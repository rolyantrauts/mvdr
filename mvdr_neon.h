#ifndef MVDR_NEON_H
#define MVDR_NEON_H

#include <complex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <arm_neon.h>
#include <fftw3.h> // Required for DOA calculation

// --- Tuning Parameters ---
#define NUM_BINS 257         
#define ALPHA 0.95f          // Smoothing factor
#define DIAG_LOAD 2e-1f      // Diagonal Loading
#define PI 3.14159265358979323846f

typedef std::complex<float> cpx;

struct MvdrState {
    // 2x2 Covariance Matrix for each frequency bin
    cpx R[NUM_BINS][2][2]; 
};

// Initialize state (zero out covariance)
inline void mvdr_init(MvdrState* st) {
    for (int k = 0; k < NUM_BINS; ++k) {
        st->R[k][0][0] = cpx(1e-4f, 0.0f); 
        st->R[k][0][1] = cpx(0.0f, 0.0f);
        st->R[k][1][0] = cpx(0.0f, 0.0f);
        st->R[k][1][1] = cpx(1e-4f, 0.0f);
    }
}

// 2x2 Matrix Inversion (Analytic)
inline void invert_2x2(cpx R[2][2], cpx invR[2][2]) {
    cpx det = R[0][0] * R[1][1] - R[0][1] * R[1][0];
    float mag = std::abs(det);
    if (mag < 1e-9f) {
        // Fallback to Identity
        invR[0][0] = 1.0f; invR[0][1] = 0.0f;
        invR[1][0] = 0.0f; invR[1][1] = 1.0f;
        return;
    }

    cpx invDet = 1.0f / det;
    invR[0][0] =  R[1][1] * invDet;
    invR[0][1] = -R[0][1] * invDet;
    invR[1][0] = -R[1][0] * invDet;
    invR[1][1] =  R[0][0] * invDet;
}

// --- GCC-PHAT DOA Calculation ---
// Returns angle in degrees (0-180) relative to array
inline float calculate_doa_gcc_phat(fftwf_complex* fft_ch1, 
                                    fftwf_complex* fft_ch2, 
                                    fftwf_complex* temp_fft_in, // Scratch buffer
                                    float* temp_time_out,       // Scratch buffer
                                    fftwf_plan plan_c2r,        // IFFT Plan
                                    float mic_dist,
                                    int frame_size,
                                    int sample_rate) {
    
    // 1. Compute PHAT Cross-Spectrum: G = X1 * conj(X2) / |X1 * conj(X2)|
    for(int k=0; k<NUM_BINS; ++k) {
        cpx x1(fft_ch1[k][0], fft_ch1[k][1]);
        cpx x2(fft_ch2[k][0], fft_ch2[k][1]);
        
        cpx cross = x1 * std::conj(x2);
        float mag = std::abs(cross);
        
        // Normalize (PHAT) - add epsilon to avoid div/0
        cpx phat = cross / (mag + 1e-9f); 
        
        // Fill scratch buffer for IFFT
        temp_fft_in[k][0] = phat.real();
        temp_fft_in[k][1] = phat.imag();
    }
    
    // 2. Inverse FFT (Frequency -> Time Correlation)
    fftwf_execute(plan_c2r); // Output goes to temp_time_out
    
    // 3. Find Peak in Correlation
    int best_idx = 0;
    float max_val = -1e9f;
    
    for(int i=0; i<frame_size; ++i) {
        if(temp_time_out[i] > max_val) {
            max_val = temp_time_out[i];
            best_idx = i;
        }
    }
    
    // 4. Convert Index to Delay (Samples)
    // FFT buffer is circular: 0..N/2 is positive, N/2..N is negative
    float delay_samples = 0;
    if (best_idx < frame_size/2) {
        delay_samples = (float)best_idx;
    } else {
        delay_samples = (float)(best_idx - frame_size);
    }
    
    // 5. Convert Delay to Angle
    float speed_sound = 343.0f;
    float time_delay = delay_samples / (float)sample_rate;
    float dist_diff = time_delay * speed_sound;
    
    // Clamp to valid range [-1, 1] for acos
    float ratio = dist_diff / mic_dist;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < -1.0f) ratio = -1.0f;
    
    // acos returns radians (0..PI) -> convert to degrees
    return acosf(ratio) * 180.0f / PI;
}

// --- Core MVDR Beamformer ---
inline void process_mvdr_neon(MvdrState* st, 
                              float* in_fft_ch1, 
                              float* in_fft_ch2, 
                              float* sv1, 
                              float* sv2, 
                              float* out_fft, 
                              int num_bins) {
    cpx* X1 = (cpx*)in_fft_ch1;
    cpx* X2 = (cpx*)in_fft_ch2;
    cpx* Y  = (cpx*)out_fft;

    for (int k = 0; k < num_bins; ++k) {
        cpx x1 = X1[k];
        cpx x2 = X2[k];

        // Covariance Update
        cpx xx00 = x1 * std::conj(x1);
        cpx xx01 = x1 * std::conj(x2);
        cpx xx10 = std::conj(xx01);
        cpx xx11 = x2 * std::conj(x2);

        st->R[k][0][0] = ALPHA * st->R[k][0][0] + (1.0f - ALPHA) * xx00;
        st->R[k][0][1] = ALPHA * st->R[k][0][1] + (1.0f - ALPHA) * xx01;
        st->R[k][1][0] = ALPHA * st->R[k][1][0] + (1.0f - ALPHA) * xx10;
        st->R[k][1][1] = ALPHA * st->R[k][1][1] + (1.0f - ALPHA) * xx11;

        // Diagonal Loading
        cpx R_loaded[2][2];
        R_loaded[0][0] = st->R[k][0][0] + DIAG_LOAD; 
        R_loaded[0][1] = st->R[k][0][1];
        R_loaded[1][0] = st->R[k][1][0];
        R_loaded[1][1] = st->R[k][1][1] + DIAG_LOAD;

        // Inversion
        cpx invR[2][2];
        invert_2x2(R_loaded, invR);

        // MVDR Weights Calculation
        cpx d1(sv1[2*k], sv1[2*k+1]); 
        cpx d2(sv2[2*k], sv2[2*k+1]);

        cpx num1 = invR[0][0] * d1 + invR[0][1] * d2;
        cpx num2 = invR[1][0] * d1 + invR[1][1] * d2;
        cpx den = std::conj(d1) * num1 + std::conj(d2) * num2;
        
        float den_mag = std::abs(den);
        if (den_mag < 1e-9f) den = 1.0f;

        cpx w1 = num1 / den;
        cpx w2 = num2 / den;

        // Apply Weights
        Y[k] = std::conj(w1) * x1 + std::conj(w2) * x2;
    }
}

#endif
