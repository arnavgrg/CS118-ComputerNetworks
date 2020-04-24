#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <iostream>
#include <string>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
using namespace std;

extern int errno;

#define PORT 3001
#define BACKLOG 5

// Global server variable to gracefully handler ctrl-c to terminate server
int server_fd;

// Define constant MIME types for response
string HTML =   "Content-Type: text/html\r\n";
string JPG =    "Content-Type: image/jpeg\r\n";
string PNG =    "Content-Type: image/png\r\n";
string TXT =    "Content-Type: text/plain\r\n";
string BINARY = "Content-Type: application/octet-stream\r\n";

string PAGE_NOT_FOUND = "HTTP/1.1 404 Not Found\r\n" + HTML + "Content-length: 307\r\n" 
                        + "\r\n" + "<!doctype HTML>\n<html>\n<head><title> 404: File Not Found\
                        </title></head>\n\n<body><h1> 404 File NOT Found.</h1><p> The requested\
                        file could not be found. Please try again.</p></body>\n</html>\n";

// Return error message after setting errno
void showError(string s) {
    perror(s.c_str());
    exit(1);
}

// Send simple 404 page html as server response
void page404(int fd) {
    write(fd, PAGE_NOT_FOUND.c_str(), PAGE_NOT_FOUND.length());
}

// handler for sigaction 
void sighandler(int s) {
    printf(" Closing server\n");
    close(s);
    exit(0);
}

// Extract file name from input buffer
string parseFileName(char* buffer) {
    const char slash = '/';

    char* pos = strchr(buffer, slash);
    int buf_len = strlen(pos);

    string name = "";
    for (int i = 1; i < buf_len; i++) {
        if (pos[i] == ' ')
            break;
        name += pos[i];
    }

    return name;
}

// Parse client's request, and serve response from server to client
void parseRequest(int client_fd) {
    char buffer[2048];
    memset(buffer, 0, 2048);

    // Read data from client into buffer
    int data_len = read(client_fd, buffer, 2048);
    if (data_len < 0) {
        showError("failed to read from client");
    }
    printf("> client request: \n%s\n", buffer);

    // Parse request to retrieve file name
    string file_name = parseFileName(buffer);
    if (file_name == "") {
        page404(client_fd);
        return;
    } 
    printf("> file name requested: %s\n\n", file_name.c_str());
    
    // If needed, change %20 to white spaces in file's name 
    for (string::size_type i = 0; (i = file_name.find("%20", i)) != string::npos; ) {
        file_name.replace(i, 3, " ");
        i += 1;
    }

    // Get file descriptor for requested file if it exists and 
    // Check for valid file descriptor
    struct stat fileStat;
    int file_fd = open(file_name.c_str(), O_RDONLY);
    if (file_fd < 0) {
        page404(client_fd);
        return;
    } if (fstat(file_fd, &fileStat) < 0) {
        showError("bad file");
        return;
    }

    close(client_fd);
    return;
}

int main () {
    int client_fd;
    
    // Initialize socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        showError("failed to initialize socket");
    }

    // Setup signal to gracefully close server fd on SIGINT
    signal (SIGINT, sighandler);

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

    while (true) {
        /*Extract the first connection on the queue of pending connections, create a new socket 
        with the same socket type protocol and address family as the specified socket, and allocate 
        a new file descriptor for that socket.*/
        if ((client_fd = accept(server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&server_addr)) < 0) {
            showError("failed to extract connection");
        }
        printf("> got connection from %s\n\n", inet_ntoa(client_addr.sin_addr));
        parseRequest(client_fd);
    }

}