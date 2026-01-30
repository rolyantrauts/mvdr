A lightweight, high-performance real-time **Minimum Variance Distortionless Response (MVDR)** beamformer written in C++ for the Raspberry Pi.

This project captures stereo audio from a 2-microphone array (e.g., ReSpeaker 2-Mic HAT), performs noise cancellation and directional beamforming, and outputs the processed audio to a **virtual loopback device**. This allows other applications (Voice Assistants, Recorders, Alexa/VoiceAI) to "hear" the cleaned audio as if it were a physical microphone.  
The Respeaker is a slave device that uses the Pi to provide the clock than be a master and supply its own, just been fighting for a day and a half to workout how to work with the clockdrift of the pi vs set rates provided by alsa.
Use a usb device and someone just hack the code to use ALSA than picking up from stdin to bypass clockdrift and using aplay to fix things.


## Features
* **Lightweight:** Optimized for Raspberry Pi Zero 2 W (ARMv8 NEON SIMD).
* **Low Latency:** Uses a Unix Pipe architecture to minimize buffering delay.
* **Robust:** Handles clock drift between cheap hardware and system clocks without clicking/glitching.
* **Virtual Microphone:** Outputs to ALSA Loopback for easy integration with other software.

---

## 1. System Requirements & Setup

### **Hardware**
* Raspberry Pi Zero 2 W (or Pi 3/4/5)
* **Microphone:** ReSpeaker 2-Mic HAT (or any Stereo USB Soundcard)
* MicroSD Card (8GB+) running **Raspberry Pi OS (Bullseye or Bookworm)**

### **Dependencies**
Update your system and install the required build tools and audio libraries (ALSA + FFTW3).

```
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential git libasound2-dev libfftw3-dev sox alsa-utils
```
Enable Loopback Kernel Module  
The "Virtual Cable" that connects the beamformer to your voice assistant is the kernel's snd-aloop module.  
Enable it temporarily (to test):`sudo modprobe snd-aloop`
Enable it permanently (on boot):`echo "snd-aloop" | sudo tee -a /etc/modules`
Verify:Run `aplay -l` and look for a card named Loopback.

## 2. CompilationClone this repository (or copy the source files):Bashgit clone <your-repo-url>
`cd mvdr_beamformer`
Build the project:  
The provided Makefile links against libasound and libfftw3f. `make clean && make`  
This produces an executable named mvdr_beamformer.  
3. Running the Beamformer (The "Magic Pipe")  
Due to clock drift issues common with budget hardware (like the ReSpeaker HAT), this application outputs Raw Audio Data to STDOUT.   
We pipe this data directly into aplay, which handles the buffering and synchronization robustly.  
Manual Test
Run this command to start the beamformer and pipe the output to the Loopback device (Side 0).  
```
### Running the Beamformer (Real-time Pipeline)

The beamformer reads from your microphone (ALSA Input), processes the audio using NEON-optimized MVDR + GCC-PHAT DOA, and writes the clean, beamformed audio to `stdout`.

To use it effectively, you typically pipe the output to `aplay` (to hear it) or to an ALSA Loopback device (to feed it into a voice assistant like rhasspy or ovos).

#### **Command Line Parameters**

* `-i [device]` : Input ALSA capture device (Default: `plughw:1,0`). Use `arecord -l` to find your mic.
* `-d [meters]` : Microphone spacing in meters.
    * **0.058** (58mm) for ReSpeaker 2-Mics.
    * **0.021** (21mm) for custom Endfire builds.
* `-g [gain]` : Digital output gain (Default: `1.0`). Useful if the MVDR math makes the audio too quiet.
* `-f [frames]` : DOA History Buffer size (Default: `100`).
    * Controls how many past frames are averaged for the "Smart Lock".
    * `100` frames ≈ 1.6 seconds of history.
* `-v` : Verbose DOA mode. Prints the current estimated angle to `stderr`.
* `-p` : Pass-through mode. Bypasses all math and sends raw Mic 1 audio to output (hardware debug).

#### **Example: Direct Playback (Testing)**
This runs the beamformer and plays the result directly to your speakers (HDMI/Headphones).

# Run with 21mm spacing, 1x gain, and verbose DOA logs

./mvdr_beamformer -i plughw:1,0 -d 0.021 -g 1.0 -v | \
aplay -f S16_LE -r 16000 -c 1
```

Example: Production Pipeline (Virtual Cable)
To send the clean audio to another program (like a Wakeword engine), use the ALSA Loopback module.
```
# 1. Load Loopback Module
sudo modprobe snd-aloop

# 2. Run Beamformer -> Loopback Side 0
./mvdr_beamformer -i plughw:1,0 -d 0.021 -g 1.0 | \
aplay -D plughw:Loopback,0,0 -c 1 -r 16000 -f S16_LE -q
```

Listening to the Output  
To verify it is working, open a second terminal and listen to the other end of the virtual cable (Loopback Side 1):  

`# Pipe the Virtual Mic (Loopback,1,0) to your Physical Speakers (Card 0)
arecord -D plughw:Loopback,1,0 -c 1 -r 16000 -f S16_LE | aplay -D plughw:0,0`  

Inter-Process Communication (IPC) & Control
The C++ beamformer listens on UDP Port 5555 for commands. This allows external programs (like a Python Wakeword script) to control the beam direction in real-time without stopping the audio stream.

Key Features:
Automatic Tracking (Default): The beamformer continuously scans using GCC-PHAT and updates the beam to follow the loudest sound source.

"Time Machine" Locking: When you send the LOCK command, the engine does not just lock to the current millisecond. It scans its internal history buffer (set by -f) to find the weighted average angle of the speech that just happened. This fixes latency issues where the wakeword detection happens after the user has stopped speaking.

UDP Commands:
RESET : Clears any locks and resumes Automatic Tracking mode.

LOCK : Locks the beam to the best angle found in the history buffer (used upon Wakeword detection).

SET [angle] : Manually forces the beam to a specific angle (e.g., SET 45).

Testing IPC (test_ipc.py)
A simple Python script is included to demonstrate how to control the beamformer. It simulates a wakeword event sequence.

```
# Open a new terminal while the beamformer is running
python3 test_ipc.py
```
test_ipc.py Workflow:

Sends RESET to ensure Auto Mode.

Waits 5 seconds (Simulating user talking).

Sends LOCK (Simulating Wakeword trigger).

Check the beamformer terminal: You will see it lock to the dominant angle from the last ~1.5s.

Waits, then sends SET 45 to demonstrate manual override.


4. Automatic Startup (Systemd Service)To make the beamformer run automatically in the background when the Pi boots:1. Create a Wrapper ScriptCreate a file named start_beamformer.sh in your project folder:Bash#!/bin/bash  

Load driver just in case
```
modprobe snd-aloop
sleep 2
```
Make it executable: `chmod +x start_beamformer.sh`  

2. Create the Service File
Create /etc/systemd/system/beamformer.service:Ini, TOML[Unit]
```
Description=MVDR Beamformer Service
After=sound.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/mvdr_beamformer
ExecStart=/home/pi/mvdr_beamformer/start_beamformer.sh
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```
(Note: Change User=pi and paths to match your username/setup)  
3. Enable and Start
```
sudo systemctl daemon-reload  
sudo systemctl enable beamformer.service
sudo systemctl start beamformer.service
```
4. Status Check `sudo systemctl status beamformer.service`  
5. Usage Arguments  
   -i [dev]Input ALSA Device (e.g., hw:1,0)plughw:1,0  
   -d [meters]Microphone spacing in meters0.058  
   -a [degrees]Steering Angle (90=Center, 0=Left, 180=Right)90.0  
   -g [float]Digital Gain Multiplier1.0  
   -p Pass-through Mode (Bypass processing for debugging)Off  
   -h Show Help Menu-Example:Bash./mvdr_beamformer -i hw:2,0 -d 0.075 -a 45 -g 1.5 | aplay ...  
   
7. Tuning TipsHollow Sound?  
Your -d (spacing) might be slightly off. Try adjusting it in small steps (e.g., 0.055, 0.060) until the voice sounds full.  
Distortion/Clipping? Check your hardware gain in alsamixer (keep it around 70-80%) or lower the software gain with -g 0.8.  
Clock Drift/Clicks? Ensure you are using the pipe method (| aplay) described above, as aplay manages buffer underruns better than direct ALSA writing on the Pi Zero.


## 2. Testing file based to emulate zero clock drift
`python3 test_offline_fine.py`

## Possible budget arrays  
The ESP32-S3 has a hardware feature specifically for this called Parallel Data Input. You can run a single clock (BCLK/WS) and read from multiple data pins simultaneously.

The Architecture: "One Clock, Two Data Lines"
You should not use I2S0 and I2S1 separately. Even if you slave the clocks, managing the DMA start times to be sample-perfect is a nightmare.

Instead, you configure I2S0 to accept 2 Data Input Lines.

Clock Domain: Single BCLK (Bit Clock) and WS (Word Select) generated by the ESP32.

Data 0: Connected to Mics 1 & 2 (Standard Stereo I2S).

Data 1: Connected to Mics 3 & 4 (Standard Stereo I2S).

The ESP32-S3's DMA engine automatically "zips" these two streams together into a single 4-channel buffer in RAM (e.g., [Ch1, Ch2, Ch3, Ch4, Ch1...]). This guarantees zero clock drift and perfect phase alignment, which is critical for MVDR

Why INMP441? They are cheap, have 24-bit precision, and handle the "L/R Select" pin natively are available and are cheap so you can set your own geometry.

The Wiring Map  
This setup treats the array as two "Stereo Pairs" sharing a heartbeat.  
Signal,ESP32-S3 Pin (Example),Mic Pair A (1 & 2),Mic Pair B (3 & 4),Note  
BCLK,GPIO 4,SCK,SCK,Shared Clock  
WS,GPIO 5,WS,WS,Shared Clock  
Data 0,GPIO 6,SD,-,Input Line 0  
Data 1,GPIO 7,-,SD,Input Line 1  
L/R,-,"Mic1=GND, Mic2=VDD","Mic3=GND, Mic4=VDD",Selects Left/Right slot  
