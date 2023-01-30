GPP = g++
MAKEDIR = mkdir -p
RM = rm

OUTPUT = test
LIB :=  -lavcodec -lffplayer -pthread
CPPFLAGS := -g3 -std=c++11 
SOURCES := $(wildcard *.cpp)
OBJECTS := $(addprefix obj/,$(notdir) $(SOURCES:.cpp=.o))

all: makedir $(OBJECTS) bin/$(OUTPUT)

makedir:
	$(MAKEDIR) obj
	$(MAKEDIR) bin

obj/%.o: %.cpp
	$(GPP) -c -o $@ $< $(CPPFLAGS)

bin/$(OUTPUT):
	$(GPP) -o $@ $(OBJECTS) $(LIB) $(CPPFLAGS)

clean:
	$(RM) $(OBJECTS) bin/$(OUTPUT)