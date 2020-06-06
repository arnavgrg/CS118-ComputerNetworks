CXX=g++
OPTS= -O2
FLAGS= -g -Wall -pthread -std=c++11 $(OPTS)
UID=304911796
CLASSES=

all: server client

server: $(CLASSES)
	$(CXX) -o $@ $^ $(FLAGS) $@.cpp 

client: $(CLASSES)
	$(CXX) -o $@ $^ $(FLAGS) $@.cpp

clean:
	rm -rf *.o *.dSYM *.file server client *.tar.gz

dist: tarball
tarball: clean
	tar -cvzf /tmp/$(UID).tar.gz --exclude=./.vagrant --exclude=./.DS_Store . && mv /tmp/$(UID).tar.gz .
