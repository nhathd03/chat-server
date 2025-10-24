
# C++ TCP Chat Server & Client

Simple C++ chat application, compatible with Windows and MacOS

<br />

## Features

### Length-prefixed protocol
Each message begins with a 4-byte header that indicates how long the message is.  
This avoids partial or concatenated messages.

### Multithreading and Concurrency
Each connected client is handled in its own thread, allowing multiple users to chat simultaneously.  
Synchronization primitives like `std::mutex` and `std::shared_mutex` ensure safe access to shared data structures such as the client list.

### Cross-Platform Networking
On Windows, the app uses Winsock2 (`Ws2_32.lib`), while on macOS/Linux it can be adapted using the Berkeley sockets API.  

<br />

## Requirements

### Windows
- [MSYS2](https://www.msys2.org/) with MinGW64 environment  
- `g++` with **C++17 or higher**  
- `Ws2_32.lib` (included in Windows SDK / MinGW)  

### macOS / Linux
- `g++` or `clang++`  
- Standard POSIX socket headers (`sys/socket.h`, `netinet/in.h`, etc.)

<br />

## How to Use

### Compile 
(add `-lws2_32` for Windows)
```bash
g++ -std=c++17 server.cpp -o server.exe
g++ -std=c++17 client.cpp -o client.exe
```
### Start the Server

`./server.exe` 

### Start the Client(s)

`./client.exe 127.0.0.1`

### Chat Commands
-   Type your messages and press **Enter** to send
-   Type `/quit` to disconnect
-   Stop the server anytime using **Ctrl + C**
    
<br />

## Notes
-   Default port: 3000
-   Max username length: 20 characters
-   Max message length: 1024 bytes
-   Each message uses a 4-byte length header for safe transmission
-   Server supports multiple concurrent clients and broadcasts messages to all connected users
