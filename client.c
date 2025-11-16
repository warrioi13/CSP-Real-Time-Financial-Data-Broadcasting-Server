#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>

int client_socket = -1;
volatile sig_atomic_t client_running = 1;
pthread_t receiver_thread;

// Signal handler for CTRL+C
void signal_handler(int signum) {
    if (signum == SIGINT) {
        if (client_running) {
            printf("\n\nAttempting to disconnect cleanly...\n");
            // Send QUIT command to server if socket is open
            if (client_socket >= 0) {
                send(client_socket, "QUIT\n", 5, 0);
            }
        }
        client_running = 0;
    }
}

// Thread to receive data from the server
void* receive_thread(void* arg) {
    char buffer[BUFFER_SIZE];
    
    while (client_running) {
        int bytes = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes <= 0) {
            if (bytes == 0) {
                printf("\nServer closed connection.\n");
            } else if (errno != EINTR) {
                // Ignore EINTR (interrupted system call)
                perror("\nReceive error");
            }
            client_running = 0;
            break;
        }
        
        buffer[bytes] = '\0';
        
        // Handle server-side forced disconnection
        if (strstr(buffer, "CLOSING_CONNECTION")) {
             client_running = 0;
             printf("Connection closed by server.\n");
             break;
        }

        // Print bell character for alerts
        if (strstr(buffer, "ALERT")) {
            printf("\a");
        }
        
        printf("%s", buffer);
        fflush(stdout);
        
        // Print the prompt back if needed
        if (client_running && strstr(buffer, "\n\n") != NULL) {
             printf("> ");
             fflush(stdout);
        }

        // Check if the server responded with a prompt
        if (client_running && bytes > 0 && buffer[bytes-2] == '>' && buffer[bytes-1] == ' ') {
             printf("> ");
             fflush(stdout);
        }
    }
    
    return NULL;
}

// Prints the command menu
void print_menu() {
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║         TRADING COMMANDS               ║\n");
    printf("╠════════════════════════════════════════╣\n");
    printf("║ BUY <symbol> <qty>   - Buy shares     ║\n");
    printf("║ SELL <symbol> <qty>  - Sell shares    ║\n");
    printf("║ PORTFOLIO            - View holdings   ║\n");
    printf("║ AVAILABLE            - List stocks     ║\n");
    printf("║ SUBSCRIBE <symbol> [t] - Price alerts  ║\n");
    printf("║ HELP                 - Show help       ║\n");
    printf("║ QUIT                 - Exit            ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("> ");
    fflush(stdout);
}

// Cleanup socket resource
void cleanup_client() {
    if (client_socket >= 0) {
        close(client_socket);
        client_socket = -1;
    }
}

int main(int argc, char* argv[]) {
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char server_ip[16] = SERVER_IP;
    
    if (argc > 1) {
        strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
    }
    
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║   STOCK TRADING CLIENT v3.0           ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\nConnecting to %s:%d...\n", server_ip, PORT);
    
    signal(SIGINT, signal_handler);
    
    // 1. Create socket
    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Configure server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        cleanup_client();
        exit(EXIT_FAILURE);
    }
    
    // 2. Connect to server
    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        cleanup_client();
        exit(EXIT_FAILURE);
    }
    
    printf("✓ Connected successfully!\n\n");
    
    // Start receiving thread
    if (pthread_create(&receiver_thread, NULL, receive_thread, NULL) != 0) {
        perror("Thread creation failed");
        cleanup_client();
        exit(EXIT_FAILURE);
    }
    
    // Give the receiver thread a moment to catch the welcome message
    usleep(500000); 
    
    // Main command loop
    while (client_running) {
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL) {
            if (client_running) {
                // EOF (Ctrl+D) - simulate quit
                send(client_socket, "QUIT\n", 5, 0);
            }
            client_running = 0;
            break;
        }
        
        buffer[strcspn(buffer, "\n")] = 0; // Remove newline
        
        if (strlen(buffer) == 0) {
            printf("> ");
            fflush(stdout);
            continue;
        }
        
        // Send command to server
        strcat(buffer, "\n"); // Add newline for server-side parsing
        if (send(client_socket, buffer, strlen(buffer), 0) < 0) {
            perror("Send failed");
            client_running = 0;
            break;
        }
        
        if (strncasecmp(buffer, "QUIT", 4) == 0) {
            // Give server a chance to process QUIT before stopping receiver
            usleep(200000); 
            client_running = 0;
            break;
        }
        
        // Wait briefly for server response before printing new prompt
        usleep(200000); 
        
        // Print next prompt if still running (the receiver thread usually handles this after a delay)
        if (client_running) {
            printf("> ");
            fflush(stdout);
        }
    }
    
    // Wait for receiver thread to finish
    pthread_join(receiver_thread, NULL);
    cleanup_client();
    
    printf("\n✓ Disconnected\n");
    return 0;
}