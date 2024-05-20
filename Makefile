CXX=$(VARS) clang++
CXXFLAGS=-c -Wall -pipe -std=c++17 \
	$(shell pkg-config --cflags glfw3) \
	$(shell pkg-config --cflags vulkan) \
	$(shell pkg-config --cflags assimp) \
	-I/opt/homebrew/include -Ivendor/include
GLSLC=glslc
GLSLFLAGS=
LDFLAGS=$(shell pkg-config --libs glfw3) \
	$(shell pkg-config --libs vulkan) \
	$(shell pkg-config --libs assimp) \
	-rpath /usr/local/lib
SRC=$(wildcard src/**/*.cpp) $(wildcard src/*.cpp)
OBJ=$(SRC:.cpp=.o)
SHADERSRC=$(wildcard shaders/**/*.glsl)
SHADEROBJ=$(SHADERSRC:.glsl=.spv)
OUT=out



.PHONY: all debug release run clean

all: debug

debug:   CXXFLAGS += -ggdb
release: CXXFLAGS += -O2 -DNDEBUG

debug release: $(OUT) $(SHADEROBJ)

$(OUT): $(OBJ)
	$(CXX) $(LDFLAGS) $(OBJ) -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

%.vert.spv: %.vert.glsl
	$(GLSLC) $(GLSLFLAGS) -fshader-stage=vert $< -o $@

%.frag.spv: %.frag.glsl
	$(GLSLC) $(GLSLFLAGS) -fshader-stage=frag $< -o $@

run: all
	./$(OUT)

clean:
	rm -f $(OBJ) $(SHADEROBJ) $(OUT)