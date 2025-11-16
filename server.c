#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

// Global variable definitions
MarketData market_data;
ClientInfo clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_socket;
volatile sig_atomic_t server_running = 1;
FILE* log_file;

// Helper function to initialize market data
void init_market_data() {
    const char* symbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN", "NFLX", "META", "NVDA", "AMD", "INTC"};
    double prices[] = {150.00, 2800.00, 300.00, 250.00, 3300.00, 450.00, 320.00, 500.00, 120.00, 45.00};
    
    pthread_mutex_init(&market_data.mutex, NULL);
    pthread_cond_init(&market_data.data_updated, NULL);
    market_data.stock_count = MAX_STOCKS;
    market_data.update_count = 0;
    
    for (int i = 0; i < MAX_STOCKS; i++) {
        strcpy(market_data.stocks[i].symbol, symbols[i]);
        market_data.stocks[i].price = prices[i];
        market_data.stocks[i].base_price = prices[i];
        market_data.stocks[i].change_percent = 0.0;
        market_data.stocks[i].volume = 1000000;
    }
    log_message("Market initialized with 10 simulated stocks");
}

// Helper function to initialize client portfolio
void init_client_portfolio(ClientInfo* client) {
    client->portfolio.wallet_balance = INITIAL_BALANCE;
    client->portfolio.total_invested = 0.0;
    client->portfolio.holding_count = 0;
    
    for (int i = 0; i < MAX_STOCKS; i++) {
        client->portfolio.holdings[i].quantity = 0;
        client->portfolio.holdings[i].avg_buy_price = 0.0;
        strcpy(client->portfolio.holdings[i].symbol, "");
        
        client->subscriptions[i].active = 0;
        client->subscriptions[i].threshold = 5.0; // Default 5% threshold
        client->subscriptions[i].buy_alert_sent = 0;
        client->subscriptions[i].sell_alert_sent = 0;
    }
}

// Helper to find stock index by symbol (case-insensitive)
int find_stock(const char* symbol) {
    for (int i = 0; i < market_data.stock_count; i++) {
        if (strcasecmp(market_data.stocks[i].symbol, symbol) == 0) {
            return i;
        }
    }
    return -1;
}

// Helper to find client holding index by symbol (case-insensitive)
int find_holding(ClientInfo* client, const char* symbol) {
    for (int i = 0; i < client->portfolio.holding_count; i++) {
        if (strcasecmp(client->portfolio.holdings[i].symbol, symbol) == 0) {
            return i;
        }
    }
    return -1;
}

// Command handler: BUY
void handle_buy(ClientInfo* client, char* symbol, int qty) {
    char msg[BUFFER_SIZE];
    
    if (qty <= 0) {
        sprintf(msg, "ERROR: Invalid quantity\n");
        send(client->socket, msg, strlen(msg), 0);
        return;
    }
    
    pthread_mutex_lock(&market_data.mutex);
    int stock_idx = find_stock(symbol);
    
    if (stock_idx < 0) {
        sprintf(msg, "ERROR: Stock %s not found\n", symbol);
        pthread_mutex_unlock(&market_data.mutex);
        send(client->socket, msg, strlen(msg), 0);
        return;
    }
    
    double price = market_data.stocks[stock_idx].price;
    double cost = price * qty;
    
    if (cost > client->portfolio.wallet_balance) {
        sprintf(msg, "ERROR: Insufficient funds. Need $%.2f, have $%.2f\n", 
                        cost, client->portfolio.wallet_balance);
        pthread_mutex_unlock(&market_data.mutex);
        send(client->socket, msg, strlen(msg), 0);
        return;
    }
    
    // Execute trade
    client->portfolio.wallet_balance -= cost;
    
    int holding_idx = find_holding(client, symbol);
    if (holding_idx >= 0) {
        Holding* h = &client->portfolio.holdings[holding_idx];
        double total_cost = (h->quantity * h->avg_buy_price) + cost;
        h->quantity += qty;
        h->avg_buy_price = total_cost / h->quantity;
        client->portfolio.total_invested += cost;
    } else {
        Holding* h = &client->portfolio.holdings[client->portfolio.holding_count];
        strcpy(h->symbol, market_data.stocks[stock_idx].symbol);
        h->quantity = qty;
        h->avg_buy_price = price;
        client->portfolio.total_invested += cost;
        client->portfolio.holding_count++;
    }
    
    pthread_mutex_unlock(&market_data.mutex);
    
    sprintf(msg, "\n✓ BOUGHT %d shares of %s at $%.2f\n"
                    "Total cost: $%.2f\n"
                    "Remaining balance: $%.2f\n\n", 
                    qty, symbol, price, cost, client->portfolio.wallet_balance);
    send(client->socket, msg, strlen(msg), 0);
    
    sprintf(msg, "Client %s bought %d %s at $%.2f", client->username, qty, symbol, price);
    log_message(msg);
}

// Command handler: SELL
void handle_sell(ClientInfo* client, char* symbol, int qty) {
    char msg[BUFFER_SIZE];
    
    if (qty <= 0) {
        sprintf(msg, "ERROR: Invalid quantity\n");
        send(client->socket, msg, strlen(msg), 0);
        return;
    }
    
    int holding_idx = find_holding(client, symbol);
    if (holding_idx < 0) {
        sprintf(msg, "ERROR: You don't own %s\n", symbol);
        send(client->socket, msg, strlen(msg), 0);
        return;
    }
    
    Holding* h = &client->portfolio.holdings[holding_idx];
    if (qty > h->quantity) {
        sprintf(msg, "ERROR: You only have %d shares of %s\n", h->quantity, symbol);
        send(client->socket, msg, strlen(msg), 0);
        return;
    }
    
    pthread_mutex_lock(&market_data.mutex);
    int stock_idx = find_stock(symbol);
    double price = market_data.stocks[stock_idx].price;
    double proceeds = price * qty;
    double cost_basis_sold = h->avg_buy_price * qty;
    double profit = proceeds - cost_basis_sold;
    
    // Execute trade
    client->portfolio.wallet_balance += proceeds;
    client->portfolio.total_invested -= cost_basis_sold; // Decrease invested amount by the cost basis of sold shares
    h->quantity -= qty;
    
    if (h->quantity == 0) {
        // Remove holding if quantity is zero by shifting array elements
        for (int i = holding_idx; i < client->portfolio.holding_count - 1; i++) {
            client->portfolio.holdings[i] = client->portfolio.holdings[i + 1];
        }
        client->portfolio.holding_count--;
    } else {
        // If holding remains, the avg_buy_price is unchanged.
    }
    
    pthread_mutex_unlock(&market_data.mutex);
    
    double pl_pct = (cost_basis_sold == 0) ? 0.0 : (profit / cost_basis_sold) * 100;
    
    sprintf(msg, "\n✓ SOLD %d shares of %s at $%.2f\n"
                    "Proceeds: $%.2f\n"
                    "Profit/Loss: %s$%.2f (%.2f%%)\n"
                    "New balance: $%.2f\n\n",
                    qty, symbol, price, proceeds,
                    profit >= 0 ? "+" : "", profit, pl_pct,
                    client->portfolio.wallet_balance);
    send(client->socket, msg, strlen(msg), 0);
    
    sprintf(msg, "Client %s sold %d %s at $%.2f (P/L: $%.2f)", 
            client->username, qty, symbol, price, profit);
    log_message(msg);
}

// Command handler: PORTFOLIO
void show_portfolio(ClientInfo* client) {
    char buffer[BUFFER_SIZE * 2];
    int offset = 0;
    
    offset += sprintf(buffer + offset, "\n╔══════════════════════════════════════════════════╗\n");
    offset += sprintf(buffer + offset, "║           PORTFOLIO - %s%-24s║\n", client->username, "");
    offset += sprintf(buffer + offset, "╚══════════════════════════════════════════════════╝\n");
    offset += sprintf(buffer + offset, "💰 Wallet: $%.2f\n", client->portfolio.wallet_balance);
    
    if (client->portfolio.holding_count == 0) {
        offset += sprintf(buffer + offset, "📊 Invested: $%.2f\n\n", 0.00);
        offset += sprintf(buffer + offset, "No holdings. Use BUY command to purchase stocks.\n");
    } else {
        pthread_mutex_lock(&market_data.mutex);
        
        offset += sprintf(buffer + offset, "Holdings:\n");
        offset += sprintf(buffer + offset, "%-6s | Qty | Avg Buy | Current | Value    | P/L\n", "Stock");
        offset += sprintf(buffer + offset, "--------------------------------------------------------\n");
        
        double total_market_value = 0;
        double total_invested_cost = 0;

        for (int i = 0; i < client->portfolio.holding_count; i++) {
            Holding* h = &client->portfolio.holdings[i];
            int stock_idx = find_stock(h->symbol);
            double current_price = market_data.stocks[stock_idx].price;
            
            double cost_basis = h->quantity * h->avg_buy_price;
            double value = h->quantity * current_price;
            double pl = value - cost_basis;
            double pl_pct = (h->avg_buy_price == 0) ? 0.0 : ((current_price - h->avg_buy_price) / h->avg_buy_price) * 100;
            
            total_market_value += value;
            total_invested_cost += cost_basis;
            
            offset += sprintf(buffer + offset, "%-6s | %3d | $%6.2f | $%6.2f | $%7.2f | %s%.2f%%\n",
                                h->symbol, h->quantity, h->avg_buy_price, current_price, 
                                value, pl >= 0 ? "+" : "", pl_pct);
        }
        
        pthread_mutex_unlock(&market_data.mutex);
        
        double total_portfolio_pl = total_market_value - total_invested_cost;
        
        offset += sprintf(buffer + offset, "--------------------------------------------------------\n");
        offset += sprintf(buffer + offset, "📊 Total Invested Cost: $%.2f\n", total_invested_cost);
        offset += sprintf(buffer + offset, "Portfolio Market Value: $%.2f\n", total_market_value);
        offset += sprintf(buffer + offset, "Total P/L: %s$%.2f\n", 
                            total_portfolio_pl >= 0 ? "+" : "", total_portfolio_pl);
    }
    
    offset += sprintf(buffer + offset, "\n");
    send(client->socket, buffer, offset, 0);
}

// Command handler: AVAILABLE
void show_available(ClientInfo* client) {
    char buffer[BUFFER_SIZE];
    int offset = 0;
    
    pthread_mutex_lock(&market_data.mutex);
    
    offset += sprintf(buffer + offset, "\n═══════ AVAILABLE STOCKS (Simulated) ═══════\n");
    offset += sprintf(buffer + offset, "%-6s | %-8s | %-6s\n", "Symbol", "Price", "Change");
    offset += sprintf(buffer + offset, "----------------------------------------\n");
    for (int i = 0; i < market_data.stock_count; i++) {
        offset += sprintf(buffer + offset, "%-6s | $%8.2f | %+.2f%%\n",
                            market_data.stocks[i].symbol, 
                            market_data.stocks[i].price,
                            market_data.stocks[i].change_percent);
    }
    offset += sprintf(buffer + offset, "════════════════════════════════════════\n");
    
    pthread_mutex_unlock(&market_data.mutex);
    
    send(client->socket, buffer, offset, 0);
}

// Command handler: SUBSCRIBE
void handle_subscribe(ClientInfo* client, char* symbol, double threshold) {
    char msg[BUFFER_SIZE];
    
    int stock_idx = find_stock(symbol);
    if (stock_idx < 0) {
        sprintf(msg, "ERROR: Stock %s not found\n", symbol);
        send(client->socket, msg, strlen(msg), 0);
        return;
    }
    
    if (threshold <= 0.0) {
        sprintf(msg, "ERROR: Threshold must be positive.\n");
        send(client->socket, msg, strlen(msg), 0);
        return;
    }

    client->subscriptions[stock_idx].active = 1;
    client->subscriptions[stock_idx].threshold = threshold;
    client->subscriptions[stock_idx].buy_alert_sent = 0;
    client->subscriptions[stock_idx].sell_alert_sent = 0;
    
    sprintf(msg, "✓ Subscribed to %s for price changes of %.1f%% or more.\n", symbol, threshold);
    send(client->socket, msg, strlen(msg), 0);
}

// Alert checker
void check_alerts(ClientInfo* client) {
    char alert[BUFFER_SIZE];
    
    pthread_mutex_lock(&market_data.mutex);
    
    for (int i = 0; i < market_data.stock_count; i++) {
        if (!client->subscriptions[i].active) continue;
        
        Stock* s = &market_data.stocks[i];
        Subscription* sub = &client->subscriptions[i];
        
        // Check for Buy Alert (Price drop greater than or equal to threshold)
        if (s->change_percent <= -sub->threshold && !sub->buy_alert_sent) {
            sprintf(alert, "\n🔔 BUY ALERT: %s at $%.2f (%.2f%% drop)\n", 
                            s->symbol, s->price, s->change_percent);
            send(client->socket, alert, strlen(alert), 0);
            sub->buy_alert_sent = 1;
            sub->sell_alert_sent = 0; // Reset sell alert after a drop
        }
        
        // Check for Sell Alert (Price rise greater than or equal to threshold)
        if (s->change_percent >= sub->threshold && !sub->sell_alert_sent) {
            sprintf(alert, "\n🔔 SELL ALERT: %s at $%.2f (%.2f%% rise)\n", 
                            s->symbol, s->price, s->change_percent);
            send(client->socket, alert, strlen(alert), 0);
            sub->sell_alert_sent = 1;
            sub->buy_alert_sent = 0; // Reset buy alert after a rise
        }
    }
    
    pthread_mutex_unlock(&market_data.mutex);
}

// Command dispatcher
void handle_command(ClientInfo* client, char* command) {
    char cmd[32], arg1[32], arg2[32];
    // Read up to three arguments
    int n = sscanf(command, "%s %s %s", cmd, arg1, arg2);
    
    if (strcasecmp(cmd, "BUY") == 0 && n == 3) {
        handle_buy(client, arg1, atoi(arg2));
    }
    else if (strcasecmp(cmd, "SELL") == 0 && n == 3) {
        handle_sell(client, arg1, atoi(arg2));
    }
    else if (strcasecmp(cmd, "PORTFOLIO") == 0 && n <= 1) {
        show_portfolio(client);
    }
    else if (strcasecmp(cmd, "AVAILABLE") == 0 && n <= 1) {
        show_available(client);
    }
    else if (strcasecmp(cmd, "SUBSCRIBE") == 0 && n >= 2) {
        double thresh = n == 3 ? atof(arg2) : 5.0;
        handle_subscribe(client, arg1, thresh);
    }
    else if (strcasecmp(cmd, "HELP") == 0 && n <= 1) {
        const char* help = 
            "\n╔═══════════════════════════════════════╗\n"
            "║         TRADING COMMANDS              ║\n"
            "╠═══════════════════════════════════════╣\n"
            "║ BUY <symbol> <qty>    - Buy stocks   ║\n"
            "║ SELL <symbol> <qty>   - Sell stocks  ║\n"
            "║ PORTFOLIO             - View holdings║\n"
            "║ AVAILABLE             - List stocks  ║\n"
            "║ SUBSCRIBE <symbol> [t] - Get alerts  ║\n"
            "║ HELP                  - This help    ║\n"
            "║ QUIT                  - Exit         ║\n"
            "╚═══════════════════════════════════════╝\n"
            "Note: [t] is optional alert threshold (e.g. 1.5)\n";
        send(client->socket, help, strlen(help), 0);
    }
    else if (strcasecmp(cmd, "QUIT") == 0 && n <= 1) {
        client->active = 0;
        const char* goodbye = "CLOSING_CONNECTION\n";
        send(client->socket, goodbye, strlen(goodbye), 0);
    }
    else {
        const char* msg = "ERROR: Invalid command or arguments. Type HELP.\n";
        send(client->socket, msg, strlen(msg), 0);
    }
}

// Producer thread function (Market simulator)
void* producer_thread(void* arg) {
    log_message("Producer thread started (Simulating Market)");
    
    while (server_running) {
        sleep(3); // Update prices every 3 seconds
        
        pthread_mutex_lock(&market_data.mutex);
        
        // Randomly update prices of 1 or 2 stocks
        for (int i = 0; i < (rand() % 2) + 1; i++) {
            int idx = rand() % market_data.stock_count;
            Stock* s = &market_data.stocks[idx];
            
            // Random change between -3.00% and +3.00%
            double change = ((rand() % 600) - 300) / 10000.0; // (-0.03 to 0.03)
            s->price *= (1 + change);
            
            // Ensure price stays positive and isn't ridiculously high
            if (s->price < 0.01) s->price = s->base_price * 0.9;
            if (s->price > s->base_price * 5) s->price = s->base_price * 2;
            
            s->change_percent = ((s->price - s->base_price) / s->base_price) * 100;
            
            char msg[128];
            sprintf(msg, "Price update: %s $%.2f (%+.2f%%)", 
                            s->symbol, s->price, s->change_percent);
            log_message(msg);
        }
        
        market_data.update_count++;
        // Signal all waiting client threads about the update
        pthread_cond_broadcast(&market_data.data_updated);
        
        pthread_mutex_unlock(&market_data.mutex);
    }
    
    log_message("Producer thread exiting");
    return NULL;
}

// Client handler thread function
void* client_handler_thread(void* arg) {
    ClientInfo* client = (ClientInfo*)arg;
    char buffer[BUFFER_SIZE];
    int last_update = 0;
    
    char msg[128];
    sprintf(msg, "Client %s connected on socket %d", client->username, client->socket);
    log_message(msg);
    
    const char* welcome = 
        "\n╔════════════════════════════════════╗\n"
        "║   STOCK TRADING SYSTEM v3.0       ║\n"
        "╚════════════════════════════════════╝\n"
        "💰 Starting balance: $100,000.00\n"
        "Type HELP for commands\n\n> ";
    send(client->socket, welcome, strlen(welcome), 0);
    
    while (server_running && client->active) {
        fd_set readfds;
        struct timeval tv = {0, 100000}; // Wait 100ms for command input
        FD_ZERO(&readfds);
        FD_SET(client->socket, &readfds);
        
        // Check for command input
        if (select(client->socket + 1, &readfds, NULL, NULL, &tv) > 0) {
            int bytes = recv(client->socket, buffer, BUFFER_SIZE - 1, 0);
            if (bytes <= 0) break; // Client disconnected or error
            
            buffer[bytes] = '\0';
            buffer[strcspn(buffer, "\r\n")] = 0; // Remove newline
            
            if (strlen(buffer) > 0) {
                handle_command(client, buffer);
                if (!client->active) break; // Handle QUIT command
            }
        }
        
        // Wait for market update (Producer/Consumer pattern)
        pthread_mutex_lock(&market_data.mutex);
        
        // Use conditional wait with a timeout to prevent infinite blocking on shutdown
        while (market_data.update_count == last_update && server_running && client->active) {
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 1; // Wait up to 1 second
            
            if (pthread_cond_timedwait(&market_data.data_updated, &market_data.mutex, &timeout) == ETIMEDOUT) {
                // Time out occurred, check server_running/client_active flags
                if (!server_running || !client->active) break;
            }
        }
        
        // Check alerts if data was actually updated
        if (market_data.update_count != last_update) {
            last_update = market_data.update_count;
            pthread_mutex_unlock(&market_data.mutex);
            check_alerts(client);
        } else {
            pthread_mutex_unlock(&market_data.mutex);
        }
    }
    
    // Cleanup on disconnect
    close(client->socket);
    client->active = 0;
    
    sprintf(msg, "Client %s disconnected", client->username);
    log_message(msg);
    
    return NULL;
}

// Logger function
void log_message(const char* message) {
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    char timestamp[26];
    ctime_r(&now, timestamp);
    timestamp[24] = '\0'; // Remove trailing newline from ctime_r
    
    fprintf(log_file, "[%s] %s\n", timestamp, message);
    fflush(log_file); // Ensure message is written immediately
    printf("[%s] %s\n", timestamp, message); // Also print to console
    
    pthread_mutex_unlock(&log_mutex);
}

// Signal handler for clean shutdown
void signal_handler(int sig) {
    log_message("Shutdown signal received");
    server_running = 0;
}

// Clean up resources
void cleanup_server() {
    log_message("Cleaning up server resources");
    
    // Signal all waiting clients to wake up and exit
    pthread_cond_broadcast(&market_data.data_updated);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            clients[i].active = 0;
            close(clients[i].socket);
        }
    }
    
    if (server_socket > 0) close(server_socket);
    
    pthread_mutex_destroy(&market_data.mutex);
    pthread_cond_destroy(&market_data.data_updated);
    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&log_mutex);
    
    if (log_file) {
        log_message("===== SERVER STOPPED =====");
        fclose(log_file);
    }
}

int main() {
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    pthread_t producer_tid;
    int next_id = 1;
    
    log_file = fopen(LOG_FILE, "a");
    if (!log_file) {
        perror("Log file error");
        exit(EXIT_FAILURE);
    }
    
    log_message("===== SERVER STARTING =====");
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe signal
    
    srand(time(NULL));
    init_market_data();
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
    }
    
    // 1. Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        log_message("ERROR: Socket creation failed");
        cleanup_server();
        exit(EXIT_FAILURE);
    }
    
    // Allow reuse of address
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Configure server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    // 2. Bind socket
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_message("ERROR: Bind failed");
        cleanup_server();
        exit(EXIT_FAILURE);
    }
    
    // 3. Listen for connections
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        log_message("ERROR: Listen failed");
        cleanup_server();
        exit(EXIT_FAILURE);
    }
    
    log_message("Server listening on port 8888");
    
    // Start producer (market simulation) thread
    pthread_create(&producer_tid, NULL, producer_thread, NULL);
    
    // Main server loop (Accepting connections)
    while (server_running) {
        fd_set readfds;
        struct timeval tv = {1, 0}; // Wait 1 second
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);
        
        // Wait for activity on the server socket
        if (select(server_socket + 1, &readfds, NULL, NULL, &tv) <= 0) continue;
        
        // 4. Accept connection
        int sock = accept(server_socket, (struct sockaddr*)&client_addr, &addr_len);
        if (sock < 0) {
            if (server_running) {
                log_message("ERROR: Accept failed");
            }
            continue;
        }
        
        // Find an empty slot for the new client
        pthread_mutex_lock(&clients_mutex);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                slot = i;
                break;
            }
        }
        
        if (slot >= 0) {
            // Initialize new client structure
            clients[slot].socket = sock;
            clients[slot].active = 1;
            clients[slot].client_id = next_id++;
            sprintf(clients[slot].username, "User%d", clients[slot].client_id);
            
            init_client_portfolio(&clients[slot]);
            
            // Start client handler thread
            pthread_create(&clients[slot].thread, NULL, client_handler_thread, &clients[slot]);
            pthread_detach(clients[slot].thread); // Detach thread to clean resources automatically
        } else {
            // Server full
            const char* msg = "ERROR: Server full. Try again later.\n";
            send(sock, msg, strlen(msg), 0);
            close(sock);
            log_message("Connection rejected: Server full");
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    
    // Wait for the producer thread to finish its loop
    pthread_join(producer_tid, NULL);
    cleanup_server();
    
    return 0;
}