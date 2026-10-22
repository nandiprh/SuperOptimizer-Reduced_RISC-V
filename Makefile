CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -Isrc
LDFLAGS = 

SRCS = src/isa.cpp src/interpreter.cpp src/testgen.cpp src/cost.cpp src/mutator.cpp src/mcmc.cpp src/enumerate.cpp src/sim.cpp src/features.cpp src/verify.cpp src/frontend.cpp src/main.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = superopt

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/features.o: src/model_data.hpp

# Learned cost model pipeline (Algorithm 4).
tools/gen_dataset: tools/gen_dataset.cpp src/isa.cpp src/features.cpp src/sim.cpp src/model_gen.hpp src/model_data.hpp
	$(CXX) $(CXXFLAGS) -o $@ tools/gen_dataset.cpp src/isa.cpp src/features.cpp src/sim.cpp

dataset: tools/gen_dataset
	mkdir -p results
	./tools/gen_dataset --rows 20000 --out results/dataset.csv --seed 1

model: dataset
	python3 tools/train_model.py --dataset results/dataset.csv --out src/model_data.hpp

clean:
	rm -f $(OBJS) $(TARGET) tools/gen_dataset

.PHONY: all clean dataset model
