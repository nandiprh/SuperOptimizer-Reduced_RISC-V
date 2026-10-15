CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -Isrc
LDFLAGS = 

SRCS = src/isa.cpp src/interpreter.cpp src/testgen.cpp src/cost.cpp src/mutator.cpp src/mcmc.cpp src/frontend.cpp src/main.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = superopt

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
