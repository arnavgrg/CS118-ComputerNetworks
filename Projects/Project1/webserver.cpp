#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <iostream>
#include <string>
using namespace std;

#define PORT 3001
#define BACKLOG 5

extern int errno;

void showError(string s) {
    perror(s.c_str());
    exit(1);
}

int main () {
    int server_fd, client_fd;
    
    // Initialize socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        showError("failed to initialize socket");
    }

    // Setup socket address info
    struct sockaddr_in server_addr, client_addr;
    memset((char *)&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    // Bind socket to host and IP
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        showError("failed to bind socket to ip:port_num");
    }

    // Wait for another connection request passively
    if (listen(server_fd, BACKLOG) < 0) {
        showError("failed to listen to new socket connections");
    }

    char buffer[2048] = {0};

    while (true) {
        /*Extract the first connection on the queue of pending connections, create a new socket 
        with the same socket type protocol and address family as the specified socket, and allocate 
        a new file descriptor for that socket.*/
        if ((client_fd = accept(server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&server_addr)) < 0) {
            showError("failed to extract connection");
        }
        printf("server: got connection from %s\n", inet_ntoa(client_addr.sin_addr));
        int data_len = read(client_fd, buffer, 2048);
        if (data_len < 0) {
            showError("failed to read from socket");
        }
        if (data_len > 0)
           printf("%s\n", buffer);
    }

}