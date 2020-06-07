#include <iostream>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <thread>
#include <chrono>
#include <ctime>
#include <sys/time.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <cmath>

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

#define FIN     1
#define SYN     2
#define ACK     4
#define ACK_FIN 5
#define ACK_SYN 6

// 10 MB
#define MAX_FILE_SIZE 10*1024*1024

int pack_size          = 524;
int max_seq_number     = 25600;
const int payload_size = 512;
unsigned int seq_num   = 0;
unsigned int ack_num   = 0;
unsigned int id_num    = 0;
int time_flag          = 0;
int window_size        = 10;

// Header struct for each RDT packet
struct header {
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t id;
    uint16_t flags;
};

// Packet struct 
struct packet {
    header pack_header;
    char data[payload_size];
};

// Object in pipelining scheme
struct pipeObj {    
    std::chrono::steady_clock::time_point time_sent;
    unsigned int seq;
    unsigned int ack;
    std::streampos current_pos;
};

typedef struct header header;
typedef struct packet packet;
typedef struct pipeObj pipeObj;

// vector for pipelining
std::vector<pipeObj> sendPipe;

void setHeader(packet &p, uint32_t seq, uint32_t ack, uint16_t id, uint16_t flg) {
    p.pack_header.seq_num = htonl(seq);
    p.pack_header.ack_num = htonl(ack);
    p.pack_header.id      = htons(id);
    p.pack_header.flags   = htons(flg);
}

// Print error 
void showError(const char *s) {
    fprintf(stderr, "%s %s", "error:", s);
    exit(EXIT_FAILURE);
}

// Signal handler for SIGPIPE
void sig_handler(int s){
    if (s == SIGPIPE) {
        std::cerr << "error: server is down\n" << std::endl;
        exit(EXIT_FAILURE);
    }
}

// Print packet data to stdout
void printPacketInfo(std::string msg, char f, uint32_t seq, uint32_t ack, uint16_t flg) {
    std::string flag = "";
    if (f == 'S' || f == 'U') {
        seq = ntohl(seq);
        ack = ntohl(ack);
        flg = ntohs(flg);
    }
    switch(flg) {
        case 0:     flag="";       break;
        case 1:     flag="FIN";     break;
        case 2:     flag="SYN";     break;
        case 4:     flag="ACK";     break;
        case 5:     flag="FIN ACK"; break;
        case 6:     flag="SYN ACK"; break;
    }
    if (msg=="RECV")
        printf("RECV %u %u %s\n", seq, ack, flag.c_str());
    else if (msg=="SEND") {
        if (f == 'S')
            printf("SEND %u %u %s\n", seq, ack, flag.c_str());
        else if (f == 'U'){
            if (flag == "") 
                printf("SEND %u %u DUP-ACK\n", seq, ack);
            else
                printf("SEND %u %u %s DUP-ACK\n", seq, ack, flag.c_str());
        }
    } 
    else if (msg=="RESEND")
        printf("RESEND %u %u %s\n", seq, ack, flag.c_str());
    else if (msg=="TIMEOUT")
        printf("TIMEOUT %u\n", seq);
}

// can set time_flag to 1 prematurely if ack is received
void timer(int t) {
    // check whether time_flag is set to 1 every ms
    std::chrono::steady_clock::time_point curr = std::chrono::steady_clock::now();
    while (time_flag != 1) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - curr).count() >= t) {
            time_flag = 1;
            break;
        }
    }
}

// Function headers
int readPacket(int socket_fd, packet &p, struct addrinfo *rp, uint32_t ack);
void convertToHostByteOrder(packet &p);
void handshake(int socket_fd, struct addrinfo* rp);
void end_connection(int socket_fd, struct addrinfo* rp);
void data_transfer(int socket_fd, struct addrinfo* rp, std::string file_name);

int main(int argc, char* argv[]) {
    // Detect if trying to write to server which has closed its read end
    signal(SIGPIPE, sig_handler);

    if (argc != 4)
        showError("incorrect arguments passed\n");

    // Parse command line arguments
    std::string hostname   = argv[1];
    std::string port_no    = argv[2];
    int port               = std::stoi(port_no);
    std::string file_name  = argv[3];

    // Check for valid port number
    if (port <= 0 || port > 65536)
        showError("invalid port number\n");

    // Setup socket address info 
    struct addrinfo hints;
    struct addrinfo *server_info, *rp;
    memset(&hints, 0, sizeof(hints));
    /* Allow IPv4 */
    hints.ai_family   = AF_INET;
    /* Datagram Socket */
    hints.ai_socktype = SOCK_DGRAM;
    /* For wildcard IP address */
    hints.ai_flags    = AI_PASSIVE;

    // Get internet address with specified port number to bind and connect socket
    int s = getaddrinfo(hostname.c_str(), argv[2], &hints, &server_info);
    if (s != 0)
        showError("failed to get addrinfo\n");

    int socket_fd;
    /* getaddrinfo() returns a list of address structures.
     Find the first valid socket */
    for (rp = server_info; rp != NULL; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        // Failed, so try next address
        if (socket_fd == -1)
            continue;
        break;
    }

    // If failed to bind socket, then report error and exit
    if (rp == NULL)
        showError("failed to bind socket\n");
    
    // Get file access mode and status flag
    int flags = fcntl(socket_fd, F_GETFL, 0);
    // Set file access mode to non-blocking
    // If we try to perform an incompatible read/write, then the system call will fail
    // and set the error to EAGAIN. 
    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    // Perform TCP 3-way handshake
    handshake(socket_fd, rp);
    // Transfer file data
    data_transfer(socket_fd, rp, file_name);
    // Close connection
    end_connection(socket_fd, rp);
}

// Function to convert from network order to host byte order
void convertToHostByteOrder(packet &p) {
    p.pack_header.seq_num = ntohl(p.pack_header.seq_num);
    p.pack_header.ack_num = ntohl(p.pack_header.ack_num);
    p.pack_header.id      = ntohs(p.pack_header.id);
    p.pack_header.flags   = ntohs(p.pack_header.flags);
}

// data receiving in stop and wait
int readPacket(int socket_fd, packet &p, struct addrinfo *rp, uint32_t ack) {
    memset(&p, 0, pack_size);
    time_flag = 0;
    // start timer for 0.5s
    std::thread timer_thread(timer, 500);

    while (time_flag == 0) {
        int recv_bytes = recvfrom(socket_fd, &p, pack_size, 0, rp->ai_addr, &rp->ai_addrlen);
        if (recv_bytes >= 0) {
            // convert packet to host byte order
            convertToHostByteOrder(p);
            // print received packet to stdout
            printPacketInfo("RECV", ' ', p.pack_header.seq_num, p.pack_header.ack_num, p.pack_header.flags);
            // expected ack is received correctly
            if (ack == p.pack_header.ack_num || p.pack_header.flags == FIN) {
                time_flag = 1;
                timer_thread.join();
                // return number of bytes received
                return recv_bytes;
            } 
            // ack received but not the expected on
            else
                continue;
        }
    }

    // packet was not received from sever within 0.5s, so need to retransmit
    timer_thread.join();
    return -1;
}

// TCP handshake 
void handshake(int socket_fd, struct addrinfo* rp) {
    // monotonic clock
    std::chrono::steady_clock::time_point start_time;

    // create data packets
    packet send_p, receive_p;
    memset(&send_p, 0, sizeof(send_p));
    memset(&receive_p, 0, sizeof(receive_p));

    // need to send packet with SYN bit set
    srand(time(NULL)+getpid());
    seq_num = rand() % max_seq_number;
    setHeader(send_p, seq_num, ack_num, id_num, SYN);

    // start timer
    start_time = std::chrono::steady_clock::now();

    while (true) {
        // If more than 10 seconds pass, then stop trying to get a response from the server
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-start_time).count() >= 10) {
            close(socket_fd);
            showError("server has not responded for 10s\n");
        }
        // Send SYN packet
        sendto(socket_fd, &send_p, pack_size, 0, rp->ai_addr, rp->ai_addrlen);
        printPacketInfo("SEND", 'S', send_p.pack_header.seq_num, send_p.pack_header.ack_num, send_p.pack_header.flags);
        // Parse any data packets received
        int recv_bytes = readPacket(socket_fd, receive_p, rp, seq_num+1);
        // If > 0, then server responded correctly and within time
        if (recv_bytes >= 0) {
            // reset timer since message was received from server
            start_time = std::chrono::steady_clock::now();
            // server will set flag to ACK_SYN on first response
            if (receive_p.pack_header.flags == ACK_SYN) {
                // seq_num becomes new ack, ack becomes seq + 1
                seq_num = receive_p.pack_header.ack_num;
                ack_num = receive_p.pack_header.seq_num + 1;
                id_num  = receive_p.pack_header.id;
                break;
            }
        } else {
            continue;
        }
    }

}

// Data transfer using sliding window
void data_transfer(int socket_fd, struct addrinfo* rp, std::string file_name) {
    // Keep track of last received acknowledge and seq number
    unsigned int receive_ack = 0;
    unsigned int last_seq    = 0;
    int temp_ack             = 4;

    std::chrono::steady_clock::time_point msg_timer;

    // create data packets
    packet send_p, receive_p;
    memset(&send_p,    0, sizeof(send_p));
    memset(&receive_p, 0, sizeof(receive_p));

    // open file in binary mode and set output position to the end of the file
    std::ifstream ifs(file_name.c_str(), std::ios::binary);
    int file_len = 0;

    // error check
    if (ifs.good()) {
        // seek to end of file
        ifs.seekg(0, ifs.end);
        // read length
        file_len = ifs.tellg();
        // if length > 10MB, close client and report error
        if (file_len > MAX_FILE_SIZE) {
            close(socket_fd);
            showError("transfer file too large\n");
        }
        // seek back to the beginning of the file
        ifs.seekg(0, ifs.beg);
    } else {
        showError("error while opening file\n");
    }

    // total number of packets that need to be sent
    int num_packets = ceil((double)file_len/payload_size);

    // reset flag to 0
    time_flag = 0;

    int i = 0;
    while (i < num_packets) {
        // clear EOF bit
        ifs.clear();
        // seek to next chunk
        ifs.seekg(i*payload_size);
        // read data into packet
        ifs.read(send_p.data, payload_size);
        // set flag to ACK
        setHeader(send_p, seq_num, ack_num, id_num, 0);
        // Send packet
        sendto(socket_fd, &send_p, ifs.gcount()+12, 0, rp->ai_addr, rp->ai_addrlen);
        // Display output
        printPacketInfo("SEND", 'S', send_p.pack_header.seq_num, send_p.pack_header.ack_num, send_p.pack_header.flags);
        // update ack num
        seq_num += ifs.gcount();
        seq_num %= max_seq_number;
        // increment i and set wait flag to true to receive an ack
        i += 1;
        int recvbytes = readPacket(socket_fd, receive_p, rp, seq_num+1);
    }
}

// Send final messages before closing connection
void end_connection(int socket_fd, struct addrinfo* rp) {
    // create timers
    std::chrono::steady_clock::time_point start, send;
    
    // flag for when first FIN is received
    int fin_client = 0;
    // set ack number to 0 for FIN message
    ack_num = 0;

    // create data packets
    packet send_p, receive_p;
    memset(&send_p, 0, sizeof(send_p));
    memset(&receive_p, 0, sizeof(receive_p));

    // set header with FIN flag and ack_num set to 0
    setHeader(send_p, seq_num, ack_num, id_num, FIN);

    // start both timers
    start = std::chrono::steady_clock::now(); //10 sec response from server
    send  = std::chrono::steady_clock::now(); //0.5 sec response from server

    // Send FIN packet to server
    sendto(socket_fd, &send_p, pack_size, 0, rp->ai_addr, rp->ai_addrlen);
    printPacketInfo("SEND", 'S', send_p.pack_header.seq_num, send_p.pack_header.ack_num, send_p.pack_header.flags);

    // wait for FIN/ACK
    while (true) {
        // 10 sec timeout
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-start).count() >= 10){
            close(socket_fd);
            showError("FIN ACK not received from server\n");
        }
        // check 0.5 sec timeout and retransmit FIN packet again incase it was lost
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-send).count() >= 0.5){
            sendto(socket_fd, &send_p, pack_size, 0, rp->ai_addr, rp->ai_addrlen);
            printPacketInfo("SEND", 'S', send_p.pack_header.seq_num, send_p.pack_header.ack_num, send_p.pack_header.flags);
            // reset sent packet timer
            send = std::chrono::steady_clock::now();
        }
        // check for datagram from server
        int recvbytes = recvfrom(socket_fd, &receive_p, pack_size, 0, rp->ai_addr, &rp->ai_addrlen); 
        if (recvbytes > 0) {
            // convert to host byte order and print pack to stdout
            convertToHostByteOrder(receive_p);
            printPacketInfo("RECV", ' ', receive_p.pack_header.seq_num, receive_p.pack_header.ack_num, receive_p.pack_header.flags);

            // reset overall timer
            start = std::chrono::steady_clock::now();

            // check if we received correct ack or fin flag 
            // drop any non-FIN packet
            if (receive_p.pack_header.ack_num == seq_num + 1 || receive_p.pack_header.flags == FIN) {
                if (receive_p.pack_header.flags == ACK_FIN || receive_p.pack_header.flags == FIN) {
                    // fin was detected to set flag to 1
                    fin_client = 1;
                    // update ack number to packet's seq number + 1
                    ack_num = receive_p.pack_header.seq_num + 1;
                    setHeader(send_p, seq_num, ack_num, id_num, ACK);
                    // send ACK to server acknowledging FIN
                    sendto(socket_fd, &send_p, pack_size, 0, rp->ai_addr, rp->ai_addrlen);
                    printPacketInfo("SEND", 'S', send_p.pack_header.seq_num, send_p.pack_header.ack_num, send_p.pack_header.flags);
                    // update timer
                    start = std::chrono::steady_clock::now();
                }
                // wait 2 seconds to close connection and terminate
                // can receive ACK/FIN/FIN ACK
                while (fin_client) {
                    // if timeout exit this function
                    if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-start).count() >= 2) {
                        // close connection with client and exit
                        close(socket_fd);
                        return;
                    }
                    // check if we receive any other packet from the server before closing
                    if (recvfrom(socket_fd, &receive_p, pack_size, 0, rp->ai_addr, &rp->ai_addrlen) > 0){
                        convertToHostByteOrder(receive_p);
                        printPacketInfo("RECV", ' ', receive_p.pack_header.seq_num, receive_p.pack_header.ack_num, receive_p.pack_header.flags);

                        // drop packet since it was not expected
                        // while waiting to close, if we receive any FIN packet from server, respond back with an ACK
                        if (receive_p.pack_header.flags == FIN) {
                            ack_num = receive_p.pack_header.seq_num + 1;
                            setHeader(send_p, seq_num, ack_num, id_num, ACK);
                            sendto(socket_fd, &send_p, pack_size, 0, rp->ai_addr, rp->ai_addrlen);
                            printPacketInfo("SEND", 'S', send_p.pack_header.seq_num, send_p.pack_header.ack_num, send_p.pack_header.flags);
                        }
                    }
                }
            }
        }
    }
}