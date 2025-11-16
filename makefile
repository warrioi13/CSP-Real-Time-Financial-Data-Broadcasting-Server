CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -O2
LDFLAGS = -pthread -lm

SERVER = server
CLIENT = client

all: $(SERVER) $(CLIENT)
	@echo "✓ Build complete!"
	@echo "---"
	@echo "1. Run server in Terminal 1: make run-server"
	@echo "2. Run client in Terminal 2: make run-client"

$(SERVER): server.c server.h
	$(CC) $(CFLAGS) -o $(SERVER) server.c $(LDFLAGS)
	@echo "✓ Server compiled"

$(CLIENT): client.c client.h
	$(CC) $(CFLAGS) -o $(CLIENT) client.c $(LDFLAGS)
	@echo "✓ Client compiled"

clean:
	rm -f $(SERVER) $(CLIENT) *.o server.log
	@echo "✓ Cleaned build files and server.log"

run-server: $(SERVER)
	@echo "Starting server..."
	./$(SERVER)

run-client: $(CLIENT)
	@echo "Starting client..."
	./$(CLIENT)

help:
	@echo "Targets:"
	@echo "  make          - Build server and client"
	@echo "  make clean    - Remove build files"
	@echo "  make run-server - Run server"
	@echo "  make run-client - Run client (optional: pass IP as argument, e.g., make run-client 192.168.1.10)"

.PHONY: all clean run-server run-client help