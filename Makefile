CXX = g++
CXXFLAGS = -O3 -Wall -pthread -march=armv8-a -mtune=cortex-a53
LDFLAGS = -lasound -lfftw3f -lm -latomic

TARGET = mvdr_beamformer
SRCS = main.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
