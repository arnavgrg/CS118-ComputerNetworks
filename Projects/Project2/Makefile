CXX=g++
OPTS= -O2
FLAGS= -g -Wall -pthread -std=c++11 $(OPTS)
UID=304911796
CL=

all: server client

server: $(CL)
	$(CXX) -o $@ $^ $(FLAGS) $@.cpp 

client: $(CL)
	$(CXX) -o $@ $^ $(FLAGS) $@.cpp

clean:
	rm -rf *.o *.dSYM *.file server client *.tar.gz

dist: 
	tar -czvf $(UID).tar.gz server.cpp client.cpp Makefile README
