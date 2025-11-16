# CSP-Real-Time-Financial-Data-Broadcasting-Server
Real-time stock data server that simulates live market feeds. A producer thread updates prices in shared memory while multiple client threads broadcast changes to connected clients. Uses synchronization for safe access, supports signal-based graceful shutdown, and logs all system events.
