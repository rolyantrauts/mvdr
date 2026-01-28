import numpy as np
import scipy.io.wavfile as wav
import subprocess
import os
import sys

# --- Configuration ---
TEST_FILE = "test5.wav"      # Your mono recording
TEMP_STEREO = "temp_src.wav" # Generated stereo file
TEMP_OUT = "temp_out.wav"    # Processed output
EXECUTABLE = "./mvdr_file"   # The C++ File Processor

MIC_DIST = 0.08575             # 85mm
FS = 16000
SPEED_SOUND = 343.0

def create_stereo_source_at_90(mono_file, out_file):
    """
    Creates a stereo file where the source is perfectly at 90 degrees (Center).
    At 90 degrees, cos(90) = 0, so delay is 0. 
    Ideally, both channels are identical.
    """
    try:
        fs, audio = wav.read(mono_file)
    except FileNotFoundError:
        print(f"[Error] {mono_file} not found. Please record it first.")
        sys.exit(1)

    # Ensure Mono
    if audio.ndim > 1: audio = audio[:, 0]
    
    # Create Stereo (Duplicate channels for 90 deg source)
    stereo = np.column_stack((audio, audio)).astype(np.int16)
    wav.write(out_file, fs, stereo)
    return fs

def calculate_rms_db(audio_float):
    """Calculate RMS level in dB"""
    rms = np.sqrt(np.mean(audio_float**2))
    return 20 * np.log10(rms + 1e-9)

def run_fine_sweep():
    print(f"1. Generating Simulation Source at 90 degrees (Center)...")
    fs = create_stereo_source_at_90(TEST_FILE, TEMP_STEREO)
    
    # Calculate Input Level
    _, src = wav.read(TEMP_STEREO)
    src_float = src[:, 0].astype(float) / 32768.0
    ref_db = calculate_rms_db(src_float)
    
    print(f"   Source Level: {ref_db:.2f} dB")
    print("\n2. Sweeping Beam Angle (Cone of Silence Test)")
    print("-" * 65)
    print(f"{'Beam Angle':<12} | {'Output (dB)':<12} | {'Attenuation':<12} | {'Visual'}")
    print("-" * 65)

    # Sweep from 0 to 180 in 3-degree steps
    for angle in range(0, 181, 3):
        
        # Run the C++ MVDR Processor
        # We assume the source is at 90, but we tell the math to look at 'angle'
        cmd = [
            EXECUTABLE, 
            "-i", TEMP_STEREO, 
            "-o", TEMP_OUT, 
            "-a", str(angle), 
            "-d", str(MIC_DIST)
        ]
        
        # Run silently
        ret = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
        if ret.returncode != 0:
            print(f"{angle:<12} | [Error] C++ Binary Failed")
            continue

        if not os.path.exists(TEMP_OUT):
            print(f"{angle:<12} | [Error] No Output File")
            continue

        # Analyze Result
        try:
            _, out_data = wav.read(TEMP_OUT)
            out_float = out_data.astype(float) / 32768.0
            out_db = calculate_rms_db(out_float)
            
            # Attenuation = How much quieter is it compared to the source?
            attenuation = ref_db - out_db
            
            # Visual Bar
            # If Atten < 3dB, it's the "Main Lobe" (Keep)
            # If Atten > 10dB, it's the "Null" (Silence)
            bar_len = int(attenuation)
            visual = "#" * bar_len
            
            # Highlight the 'Pass' region
            marker = ""
            if angle == 90: marker = "<-- TARGET"
            
            print(f"{angle:<12} | {out_db:<12.2f} | {attenuation:<12.2f} | {visual} {marker}")

        except Exception as e:
            print(f"{angle:<12} | [Analysis Error] {e}")

    # Cleanup
    if os.path.exists(TEMP_STEREO): os.remove(TEMP_STEREO)
    if os.path.exists(TEMP_OUT): os.remove(TEMP_OUT)

if __name__ == "__main__":
    if not os.path.exists(EXECUTABLE):
        print("Error: ./mvdr_file not found. Did you run 'make clean && make'?")
    else:
        run_fine_sweep()
