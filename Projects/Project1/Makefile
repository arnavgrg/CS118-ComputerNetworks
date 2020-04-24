UID=304911796

default:
	g++ -Wall -Wextra -g -o server webserver.cpp

dist:
	tar -czvf $(UID).tar.gz webserver.cpp Makefile README

clean:
	rm -rf server
	# rm -rf server.dSYM
	# rm $(UID).tar.gaz