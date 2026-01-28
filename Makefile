CXX = g++
CXXFLAGS = -O3 -Wall -pthread -march=armv8-a -mtune=cortex-a53
LIBS = -lasound -lfftw3f -lm -latomic

# Define all targets here
all: mvdr_beamformer mvdr_file

# Target 1: The Live Streamer (Pipe Mode)
mvdr_beamformer: main.cpp mvdr_neon.h
	$(CXX) $(CXXFLAGS) -o mvdr_beamformer main.cpp $(LIBS)

# Target 2: The File Processor (Offline Test)
mvdr_file: main_file.cpp mvdr_neon.h
	$(CXX) $(CXXFLAGS) -o mvdr_file main_file.cpp $(LIBS)

# Cleanup
clean:
	rm -f mvdr_beamformer mvdr_file
