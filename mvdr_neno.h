#ifndef MVDR_NEON_H
#define MVDR_NEON_H

#include <complex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <arm_neon.h>

// --- Tuning Parameters ---
#define NUM_BINS 257         // For FRAME_SIZE 512
#define ALPHA 0.95f          // Smoothing factor (Higher = slower adaptation, more stable)
#define DIAG_LOAD 2e-1f      // Diagonal Loading (0.2). Increase if volume is still crazy.

// Complex number structure compatible with NEON/FFTW layout
typedef std::complex<float> cpx;

struct MvdrState {
    // 2x2 Covariance Matrix for each frequency bin
    // R[bin][row][col]
    cpx R[NUM_BINS][2][2]; 
};

// Initialize state (zero out covariance)
inline void mvdr_init(MvdrState* st) {
    for (int k = 0; k < NUM_BINS; ++k) {
        st->R[k][0][0] = cpx(1e-4f, 0.0f); // Small init to prevent div/0
        st->R[k][0][1] = cpx(0.0f, 0.0f);
        st->R[k][1][0] = cpx(0.0f, 0.0f);
        st->R[k][1][1] = cpx(1e-4f, 0.0f);
    }
}

// 2x2 Matrix Inversion (Analytic solution is faster than generic solver)
inline void invert_2x2(cpx R[2][2], cpx invR[2][2]) {
    // Determinant = ad - bc
    cpx det = R[0][0] * R[1][1] - R[0][1] * R[1][0];
    
    // Check for singularity (shouldn't happen with Diagonal Loading)
    float mag = std::abs(det);
    if (mag < 1e-9f) {
        // Fallback to Identity if singular
        invR[0][0] = 1.0f; invR[0][1] = 0.0f;
        invR[1][0] = 0.0f; invR[1][1] = 1.0f;
        return;
    }

    cpx invDet = 1.0f / det;

    // Inverse:
    // [ d  -b ]
    // [ -c  a ]  * (1/det)
    invR[0][0] =  R[1][1] * invDet;
    invR[0][1] = -R[0][1] * invDet;
    invR[1][0] = -R[1][0] * invDet;
    invR[1][1] =  R[0][0] * invDet;
}

// Main Processing Function
inline void process_mvdr_neon(MvdrState* st, 
                              float* in_fft_ch1, 
                              float* in_fft_ch2, 
                              float* sv1, 
                              float* sv2, 
                              float* out_fft, 
                              int num_bins) {
    
    // Cast float arrays to complex pointers for easier math
    cpx* X1 = (cpx*)in_fft_ch1;
    cpx* X2 = (cpx*)in_fft_ch2;
    cpx* Y  = (cpx*)out_fft;

    for (int k = 0; k < num_bins; ++k) {
        // 1. Get current input vector X = [x1, x2]
        cpx x1 = X1[k];
        cpx x2 = X2[k];

        // 2. Update Covariance Matrix (Recursive Averaging)
        // R_new = alpha * R_old + (1-alpha) * (X * X')
        
        // Term: X * X^H (Outer product)
        cpx xx00 = x1 * std::conj(x1); // |x1|^2
        cpx xx01 = x1 * std::conj(x2);
        cpx xx10 = std::conj(xx01);    // Hermitian symmetry
        cpx xx11 = x2 * std::conj(x2); // |x2|^2

        // Apply Smoothing
        st->R[k][0][0] = ALPHA * st->R[k][0][0] + (1.0f - ALPHA) * xx00;
        st->R[k][0][1] = ALPHA * st->R[k][0][1] + (1.0f - ALPHA) * xx01;
        st->R[k][1][0] = ALPHA * st->R[k][1][0] + (1.0f - ALPHA) * xx10;
        st->R[k][1][1] = ALPHA * st->R[k][1][1] + (1.0f - ALPHA) * xx11;

        // 3. Apply Diagonal Loading (Regularization)
        // This is the key fix for the volume jump!
        // We create a temporary loaded matrix for inversion.
        cpx R_loaded[2][2];
        R_loaded[0][0] = st->R[k][0][0] + DIAG_LOAD; 
        R_loaded[0][1] = st->R[k][0][1];
        R_loaded[1][0] = st->R[k][1][0];
        R_loaded[1][1] = st->R[k][1][1] + DIAG_LOAD;

        // 4. Invert Covariance Matrix
        cpx invR[2][2];
        invert_2x2(R_loaded, invR);

        // 5. Calculate MVDR Weights
        // w = (invR * d) / (d' * invR * d)
        
        // Steering Vector d
        // We handle sv as float* but treat pairs as real/imag
        cpx d1(sv1[2*k], sv1[2*k+1]); 
        cpx d2(sv2[2*k], sv2[2*k+1]);

        // Numerator: invR * d
        cpx num1 = invR[0][0] * d1 + invR[0][1] * d2;
        cpx num2 = invR[1][0] * d1 + invR[1][1] * d2;

        // Denominator: d' * (invR * d)
        // d' is conjugate transpose of d
        cpx den = std::conj(d1) * num1 + std::conj(d2) * num2;
        
        // Avoid division by zero
        float den_mag = std::abs(den);
        if (den_mag < 1e-9f) den = 1.0f;

        // Weights
        cpx w1 = num1 / den;
        cpx w2 = num2 / den;

        // 6. Apply Weights to Input (Beamforming)
        // y = w' * x = conj(w1)*x1 + conj(w2)*x2
        Y[k] = std::conj(w1) * x1 + std::conj(w2) * x2;
    }
}

#endif
