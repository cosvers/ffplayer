GPP = g++
MAKEDIR = mkdir -p
RM = rm

OUTPUT = test
INCLUDE = ../
LIB := -lffplayer -pthread
CFLAGS := -g3 -std=c++11 
SOURCES := $(wildcard *.cpp)
OBJECTS := $(addprefix obj/,$(notdir) $(SOURCES:.cpp=.o))

all: makedir $(OBJECTS) bin/$(OUTPUT)

makedir:
	$(MAKEDIR) obj
	$(MAKEDIR) bin

obj/%.o: %.cpp
	$(GPP) -I $(INCLUDE) -c -o $@ $< $(CFLAGS)

bin/$(OUTPUT):
	$(GPP) -o $@ $(OBJECTS) -L$(INCLUDE) $(LIB) $(CFLAGS)

clean:
	$(RM) $(OBJECTS) bin/$(OUTPUT)