#ifndef CLIENT_H
#define CLIENT_H

#include <signal.h>

// Constants
#define SERVER_IP "127.0.0.1"
#define PORT 8888
#define BUFFER_SIZE 1024

// Function Prototypes
void signal_handler(int signum);
void* receive_thread(void* arg);
void print_menu();
void cleanup_client();

#endif