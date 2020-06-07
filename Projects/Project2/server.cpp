#include <iostream>
#include <thread>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <stdlib.h>
#include <fstream>
#include <map>
#include <ctime>

#define FIN     1
#define SYN     2
#define ACK     4
#define ACK_FIN 5
#define ACK_SYN 6

// connection timeout and packets
//const unsigned int overall_timeout = 10;
const int payload_size        = 512;
const int allowed_connections = 20;
const int max_seq_num         = 25600;

// timeval structs for timers
struct timeval tv;
struct timeval ts_tv;
// for file information
struct stat file_info;

// Print error 
void showError(const char *s) {
    fprintf(stderr, "%s %s", "error:", s);
    exit(EXIT_FAILURE);
}

// Signal handler
void sighandler(int s) {
    if (s == SIGINT || s == SIGQUIT || s == SIGTERM) {
        fprintf(stderr, "%s", "closing server\n");
        close(s);
        exit(EXIT_SUCCESS);
    }
}

// Header struct for each RDT packet
struct header {
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t id;
    uint16_t flags;
};
typedef struct header header;

// Packet struct 
struct packet {
    header pack_header;
    char data[payload_size];
};
typedef struct packet packet;

// Connection info struct
struct conn_info {
    packet pack;
    int isFin;
    struct sockaddr src_addr;
    socklen_t addr_len;
    std::ofstream file;
    clock_t t_stamp;
};
typedef struct conn_info conn_info;

// track connections
conn_info connections[allowed_connections];
// track timestamp of connections for each connection ID
std::map<int, time_t> last_t_stamp;

bool isTimeout(clock_t t_stamp, int conn_id) {
    // CLOCKS_PER_SEC for 32 bit machine ~ 1,000,000 
    if ( ((clock()-t_stamp)/CLOCKS_PER_SEC > 9) && (conn_id != 0))
          return true;
    return false;
}

void printPacketInfo(std::string msg, packet p) {
    std::string flag;
    switch (p.pack_header.flags) {
        case 0:     flag=" ";       break;
        case 1:     flag="FIN";     break;
        case 2:     flag="SYN";     break;
        case 4:     flag="ACK";     break;
        case 5:     flag="FIN ACK"; break;
        case 6:     flag="SYN ACK"; break;
    }
    uint32_t seq_num = p.pack_header.seq_num;
    uint32_t ack_num = p.pack_header.ack_num;
    // RECV/SEND/RESEND <seqNum> <AckNum> [SYN] [FIN] [ACK]
    // Resend Scenarios:
    //      Server FIN loss or client FIN-ACK loss
    //      FIN-ACK loss from the client->server
    if (msg=="RECV" || msg=="SEND" || msg=="RESEND")
        std::cout << msg << " " << seq_num << " " << ack_num << " " << flag << std::endl;
    // Timeout has separate output format
    else if (msg=="TIMEOUT")
        std::cout << msg << " " << seq_num << std::endl;
}

// Helper method to update buffer fields
void updateBuffer(packet &buffer, int i) {
    buffer.pack_header.seq_num = htonl(connections[i].pack.pack_header.seq_num);
    buffer.pack_header.ack_num = htonl(connections[i].pack.pack_header.ack_num);
    buffer.pack_header.id      = htons(connections[i].pack.pack_header.id);
    buffer.pack_header.flags   = htons(connections[i].pack.pack_header.flags);
}

// Set fields to host byte order
void changeByteOrder(packet &buffer) {
    buffer.pack_header.seq_num = ntohl(buffer.pack_header.seq_num);
    buffer.pack_header.ack_num = ntohl(buffer.pack_header.ack_num);
    buffer.pack_header.id      = ntohs(buffer.pack_header.id);
    buffer.pack_header.flags   = ntohs(buffer.pack_header.flags);
}

int main(int argc, char* argv[]) {
    // Setup signal handler
    if (signal(SIGINT, sighandler) == SIG_ERR) 
        showError("could not setup signal handler\n");
    
    // Ensure port number is passed in as argument
    if (argc != 2)
        showError("provide port number - ./server <port_no>\n");
    
    // Parse port number from command line
    int port_no = atoi(argv[1]);
    if (port_no <= 0 || port_no > 65536)
        showError("invalid port number\n");

    // Setup socket address info 
    struct addrinfo hints;
    struct addrinfo *server_info, *rp;
    memset(&hints, 0, sizeof(hints));
    /* Allow IPv4 */
    hints.ai_family = AF_INET;
    /* Datagram Socket */
    hints.ai_socktype = SOCK_DGRAM;
    /* For wildcard IP address */
    hints.ai_flags = AI_PASSIVE;

    // Get internet address with specified port number to bind and connect socket
    int s = getaddrinfo(NULL, argv[1], &hints, &server_info);
    if (s != 0)
        showError("failed to get addrinfo\n");

    int socket_fd;
    /* getaddrinfo() returns a list of address structures.
     Try each address until we successfully bind. */
    for (rp = server_info; rp != NULL; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        // Failed, so try next address
        if (socket_fd == -1)
            continue;
        // Set socket options and the socket level
        int opt = 1;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int));
        // Assign address to unbound socket
        if (bind(socket_fd, rp->ai_addr, rp->ai_addrlen) < 0){
            close(socket_fd);
            continue;
        }
        break;
    }

    // If failed to bind socket, then report error and exit
    if (rp == NULL)
        showError("failed to bind socket\n");
    
    // free server addrinfo struct
    freeaddrinfo(server_info);

    // Setup struct to read datagrams being sent by clients
    struct sockaddr client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    socklen_t client_addr_len = sizeof(client_addr);
    // Initialize buffer
    packet buffer;
    memset(&buffer, 0, sizeof(buffer));

    // Server is ready to receive datagrams from all clients
    while (true) {
        
        // Clear buffer
        memset(&buffer, 0, sizeof(buffer));
        // Receive data 
        ssize_t recv_bytes = recvfrom(socket_fd, &buffer, sizeof(buffer), 0, &client_addr, &client_addr_len);
        // If recv_bytes is 0, then theres no messages available so just loop back
        if (recv_bytes == (ssize_t)0)
            continue;
        else if (recv_bytes < 0)
            showError("recvfrom returned -1");
        
        // Convert to host byte order
        changeByteOrder(buffer);
        
        // Log received packet to stdout
        printPacketInfo("RECV", buffer);

        // Handle SYN flag
        if (buffer.pack_header.flags == SYN) {
            for (int i=0; i<allowed_connections; i++) {
                if ((connections[i].src_addr.sa_family != client_addr.sa_family) && 
                                    (connections[i].pack.pack_header.id == 0)) {
                    // Setup correct file path based on connection ID
                    std::string file_path = "./" + std::to_string(i+1) + ".file";

                    /* update connection fields */
                    // Set flag to SYN ACK
                    connections[i].pack.pack_header.flags = 6;
                    // New ack number is current seq number + 1
                    connections[i].pack.pack_header.ack_num = buffer.pack_header.seq_num + 1;
                    // Initialize random sequence number
                    srand(time(NULL)+getpid());
                    int seqNum = rand() % max_seq_num;
                    connections[i].pack.pack_header.seq_num = seqNum;
                    // Increment packet header ID
                    connections[i].pack.pack_header.id = i + 1;
                    connections[i].src_addr = client_addr;
                    connections[i].addr_len = client_addr_len;
                    // Open file to store data in
                    connections[i].file.open(file_path);
                    
                    /* update buffer fields */
                    updateBuffer(buffer, i);

                    // send message to client //
                    sendto(socket_fd, &buffer, sizeof(connections[i].pack), 0, &connections[i].src_addr, connections[i].addr_len);
                    printPacketInfo("SEND", connections[i].pack);

                    break;
                }
                else {
                    sendto(socket_fd, &buffer, sizeof(connections[i].pack), 0, &connections[i].src_addr, connections[i].addr_len);
                }
            }
        }
        
        // Handle ACK flag
        /* Find connection with same ID as in the packet, compare seq_num with ack_num. If they
        are matching, then the packet arrived in order. Send packet back with id, updated ack and 
        seq numbers */
        else if (buffer.pack_header.flags == ACK || buffer.pack_header.flags == 0) {
            for (int i=0; i<allowed_connections; i++) {
                if (buffer.pack_header.id == connections[i].pack.pack_header.id) {
                    // If fin flag, then close/save file
                    if (connections[i].isFin) {
                        connections[i].file.close();
                        break;
                    }
                    // Packet arrived in order
                    if (buffer.pack_header.seq_num == connections[i].pack.pack_header.ack_num) {
                        // Increment ack number by payload size - 12 bytes for header
                        connections[i].pack.pack_header.ack_num += recv_bytes-12;
                        // Incase it overflows past maximum, start counting from 0
                        connections[i].pack.pack_header.ack_num %= max_seq_num;

                        // packet to client it lost, need to resend
                        if (buffer.pack_header.ack_num == connections[i].pack.pack_header.seq_num) {} 
                        // packet sent to client is in order
                        else if (buffer.pack_header.ack_num == connections[i].pack.pack_header.seq_num + 1)
                            connections[i].pack.pack_header.seq_num += 1;
                        //write data to file and clear write buffer
                        connections[i].file.write(buffer.data, recv_bytes-12);
                        connections[i].file.flush();
                    }
                    // Packet arrived out of order
                    else {
                        // Packet to client is lost, need to resend
                        if (buffer.pack_header.ack_num == connections[i].pack.pack_header.seq_num) {}
                        // Packet sent to client is in order
                        else if (buffer.pack_header.ack_num == connections[i].pack.pack_header.seq_num + 1)
                            connections[i].pack.pack_header.seq_num += 1;
                    }
                    // Set flag to ack
                    connections[i].pack.pack_header.flags = 4;
                    
                    // Update buffer fields
                    updateBuffer(buffer, i);

                    // send message to client
                    sendto(socket_fd, &buffer, sizeof(connections[i].pack), 0, &connections[i].src_addr, connections[i].addr_len);
                    printPacketInfo("SEND", connections[i].pack);
                    break;
                }
            }
        } 
        
        // Handle FIN flag
        else if (buffer.pack_header.flags == FIN) {
            for (int i=0; i<allowed_connections; i++) {
                if (buffer.pack_header.id == connections[i].pack.pack_header.id) {
                    // Packet arrived in order
                    if (buffer.pack_header.seq_num == connections[i].pack.pack_header.ack_num)
                        connections[i].pack.pack_header.ack_num += 1;
                    // Packet arrived out of order
                    else {}
                    
                    // Update connection flag
                    connections[i].pack.pack_header.flags = 4;
                    
                    // Update buffer fields
                    updateBuffer(buffer, i);
                    
                    // send ACK message to client
                    sendto(socket_fd, &buffer, sizeof(connections[i].pack), 0, &connections[i].src_addr, connections[i].addr_len);
                    printPacketInfo("SEND", connections[i].pack);

                    // Update buffer for FIN message
                    buffer.pack_header.seq_num = htonl(connections[i].pack.pack_header.seq_num);
                    uint32_t ack_num = 0;
                    buffer.pack_header.ack_num = htonl(ack_num);
                    buffer.pack_header.id      = htons(connections[i].pack.pack_header.id);
                    uint16_t fin = 1;
                    buffer.pack_header.flags   = htons(fin);

                    // send ACK message to client
                    sendto(socket_fd, &buffer, sizeof(connections[i].pack), 0, &connections[i].src_addr, connections[i].addr_len);
                    connections[i].isFin = 1;

                    // convert back to host byte order so we can print it
                    changeByteOrder(buffer);

                    printPacketInfo("SEND", buffer);
                    break;
                }
            }
        }
    }
}