# Makefile for the FFMPEG ffplay C project.

GPP = g++
GCC = gcc
MAKEDIR = mkdir -p
RM = rm

PROG_NAME = ffplayer
OBJ_NAME = lib$(PROG_NAME).so
VERSION = 1
OUTPUT = $(OBJ_NAME).$(VERSION)
INSTALL_PATH = /usr/lib
INCLUDE_PATH = /usr/include

INCLUDE = -I .
LIB := -lswscale -lavcodec -lavdevice -lavformat -lavutil -lpostproc -lswresample -lavfilter -lSDL2 -lSDL2main
CPPFLAGS := $(INCLUDE) -g3 -std=c++11 
CFLAGS := $(INCLUDE) -g3 -std=c11 
SOURCES := $(wildcard *.cpp)
C_SOURCES := $(wildcard *.c)
OBJECTS := $(addprefix obj/,$(notdir) $(SOURCES:.cpp=.o))
C_OBJECTS := $(addprefix obj/,$(notdir) $(C_SOURCES:.c=.o))

all: makedir $(OBJECTS) $(C_OBJECTS) $(OUTPUT)

makedir:
	$(MAKEDIR) obj

obj/%.o: %.cpp
	$(GPP) -Wall -fPIC -c -o $@ $< $(CPPFLAGS)

obj/%.o: %.c
	$(GCC) -Wall -fPIC -c -o $@ $< $(CFLAGS)

$(OUTPUT):
	$(GPP) -shared -o $@ $(OBJECTS) $(C_OBJECTS) $(LIB)

install:
	cp $(OUTPUT) $(INSTALL_PATH)
	cp $(PROG_NAME).h $(INCLUDE_PATH)
	cd $(INSTALL_PATH)
	ln -s -f $(INSTALL_PATH)/$(OUTPUT) $(INSTALL_PATH)/$(OBJ_NAME)

clean:
	$(RM) $(OBJECTS) $(C_OBJECTS) ${OUTPUT}
	