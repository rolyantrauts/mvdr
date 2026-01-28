Here is the full content formatted as a raw file block. You can click "Copy" and paste this directly into your GitHub README.md file editor.Markdown# MVDR Beamformer for Raspberry Pi

A lightweight, high-performance real-time **Minimum Variance Distortionless Response (MVDR)** beamformer written in C++ for the Raspberry Pi.

This project captures stereo audio from a 2-microphone array (e.g., ReSpeaker 2-Mic HAT), performs noise cancellation and directional beamforming, and outputs the processed audio to a **virtual loopback device**. This allows other applications (Voice Assistants, Recorders, Alexa/VoiceAI) to "hear" the cleaned audio as if it were a physical microphone.

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

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential git libasound2-dev libfftw3-dev sox alsa-utils
Enable Loopback Kernel ModuleThe "Virtual Cable" that connects the beamformer to your voice assistant is the kernel's snd-aloop module.Enable it temporarily (to test):Bashsudo modprobe snd-aloop
Enable it permanently (on boot):Bashecho "snd-aloop" | sudo tee -a /etc/modules
Verify:Run aplay -l and look for a card named Loopback.2. CompilationClone this repository (or copy the source files):Bashgit clone <your-repo-url>
cd mvdr_beamformer
Build the project:The provided Makefile links against libasound and libfftw3f.Bashmake clean && make
This produces an executable named mvdr_beamformer.3. Running the Beamformer (The "Magic Pipe")Due to clock drift issues common with budget hardware (like the ReSpeaker HAT), this application outputs Raw Audio Data to STDOUT. We pipe this data directly into aplay, which handles the buffering and synchronization robustly.Manual TestRun this command to start the beamformer and pipe the output to the Loopback device (Side 0).Bash# -i hw:1,0      : Your Physical Mic (Check 'arecord -l')
# -d 0.058       : Mic spacing in meters (58mm for ReSpeaker)
# -a 90          : Beam Angle (90 = Center)
# | aplay ...    : The player that writes to the virtual cable

./mvdr_beamformer -i hw:1,0 -d 0.058 -a 90 | \
aplay -D plughw:Loopback,0,0 -c 1 -r 16000 -f S16_LE -q
Listening to the OutputTo verify it is working, open a second terminal and listen to the other end of the virtual cable (Loopback Side 1):Bash# Pipe the Virtual Mic to your Physical Speakers/HDMI (Card 0)
arecord -D plughw:Loopback,1,0 -c 1 -r 16000 -f S16_LE | aplay -D plughw:0,0
4. Automatic Startup (Systemd Service)To make the beamformer run automatically in the background when the Pi boots:1. Create a Wrapper ScriptCreate a file named start_beamformer.sh in your project folder:Bash#!/bin/bash
# Load driver just in case
modprobe snd-aloop
sleep 2

# Run Pipeline (Adjust -i hw:X,Y to match your mic)
/home/pi/mvdr_beamformer/mvdr_beamformer -i hw:1,0 -d 0.058 -a 90 | \
aplay -D plughw:Loopback,0,0 -c 1 -r 16000 -f S16_LE -q
Make it executable:Bashchmod +x start_beamformer.sh
2. Create the Service FileCreate /etc/systemd/system/beamformer.service:Ini, TOML[Unit]
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
(Note: Change User=pi and paths to match your username/setup)3. Enable and StartBashsudo systemctl daemon-reload
sudo systemctl enable beamformer.service
sudo systemctl start beamformer.service
4. Status CheckBashsudo systemctl status beamformer.service
5. Usage ArgumentsFlagDescriptionDefault-i [dev]Input ALSA Device (e.g., hw:1,0)plughw:1,0-d [meters]Microphone spacing in meters0.058-a [degrees]Steering Angle (90=Center, 0=Left, 180=Right)90.0-g [float]Digital Gain Multiplier1.0-pPass-through Mode (Bypass processing for debugging)Off-hShow Help Menu-Example:Bash./mvdr_beamformer -i hw:2,0 -d 0.075 -a 45 -g 1.5 | aplay ...
6. Tuning TipsHollow Sound? Your -d (spacing) might be slightly off. Try adjusting it in small steps (e.g., 0.055, 0.060) until the voice sounds full.Distortion/Clipping? Check your hardware gain in alsamixer (keep it around 70-80%) or lower the software gain with -g 0.8.Clock Drift/Clicks? Ensure you are using the pipe method (| aplay) described above, as aplay manages buffer underruns better than direct ALSA writing on the Pi Zero.
