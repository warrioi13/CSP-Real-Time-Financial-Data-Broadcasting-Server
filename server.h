#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>

// Constants
#define PORT 8888
#define MAX_CLIENTS 10
#define MAX_STOCKS 10
#define BUFFER_SIZE 1024
#define INITIAL_BALANCE 100000.00
#define LOG_FILE "server.log"

// Structures
typedef struct {
    char symbol[6];
    double price;
    double base_price;
    double change_percent;
    int volume;
} Stock;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t data_updated;
    Stock stocks[MAX_STOCKS];
    int stock_count;
    int update_count;
} MarketData;

typedef struct {
    char symbol[6];
    int quantity;
    double avg_buy_price;
} Holding;

typedef struct {
    double wallet_balance;
    double total_invested;
    int holding_count;
    Holding holdings[MAX_STOCKS];
} Portfolio;

typedef struct {
    int active;
    double threshold; // Percentage change threshold for alert
    int buy_alert_sent;
    int sell_alert_sent;
} Subscription;

typedef struct {
    int client_id;
    int socket;
    char username[32];
    int active;
    pthread_t thread;
    Portfolio portfolio;
    Subscription subscriptions[MAX_STOCKS];
} ClientInfo;

// Function prototypes
void log_message(const char* message);

#endif