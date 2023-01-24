# Makefile for the FFMPEG ffplay C++ project.

GPP = g++
GCC = gcc
MAKEDIR = mkdir -p
RM = rm

OUTPUT = libffplayer.so
INCLUDE = -I .
LIB := -lswscale -lavcodec -lavdevice -lavformat -lavutil -lpostproc -lswresample -lavfilter -lSDL2 -lSDL2main
CFLAGS := $(INCLUDE) -g3 -std=c++11 
SOURCES := $(wildcard *.cpp)
C_SOURCES := $(wildcard *.c)
OBJECTS := $(addprefix obj/,$(notdir) $(SOURCES:.cpp=.o))
C_OBJECTS := $(addprefix obj/,$(notdir) $(C_SOURCES:.c=.o))

all: makedir $(OBJECTS) $(C_OBJECTS) $(OUTPUT)

makedir:
	$(MAKEDIR) obj

obj/%.o: %.cpp
	$(GPP) -Wall -fPIC -c -o $@ $< $(CFLAGS)

obj/%.o: %.c
	$(GCC) -Wall -fPIC -c -o $@ $< -g3 $(INCLUDE) -std=c11

%.so:
	$(GPP) -shared -o $@ $(OBJECTS) $(C_OBJECTS) $(LIB)

clean:
	$(RM) $(OBJECTS) $(C_OBJECTS) ${OUTPUT}
	