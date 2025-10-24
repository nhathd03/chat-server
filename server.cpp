#if defined(_WIN32)
#define _WIN32_WINNT 0x0600  
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <signal.h>

#pragma comment(lib, "Ws2_32.lib")
constexpr const char* DEFAULT_PORT = "3000";

#else
#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <signal.h>
#include <cstring>
#include <fcntl.h>

#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_BOTH SHUT_RDWR
#define closesocket close
#define ioctlsocket ioctl
#define ZeroMemory(b, s) memset(b, 0, s)
constexpr const char* DEFAULT_PORT = "3000";

#endif


struct client_info {
    SOCKET socket;
    std::string name;
};

std::vector<std::shared_ptr<client_info>> clients_list;
std::shared_mutex client_mutex;
std::atomic<bool> running = true;


/**
 * @brief Constructs a length-prefixed message buffer
 * 
 * Format: [4-byte length (network byte order)][message payload]
 * 
 * @param payload The message string to send
 * @return Vector containing the complete message buffer
 */
inline std::vector<char> build_message_buffer(const std::string& payload) {
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::vector<char> buffer(sizeof(len) + payload.size());

    memcpy(buffer.data(), &len, sizeof(len));
    memcpy(buffer.data() + sizeof(len), payload.data(), payload.size());

    return buffer;
}

/**
 * @brief Receive exact number of bytes, handling partial receives
 * @param sock Socket to receive from
 * @param buf Buffer to store received data
 * @param len Number of bytes to receive
 * @return Total bytes received, or <= 0 on error/disconnect
 *         
 */    
int recv_all(SOCKET sock, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) return (total == 0) ? n : total;
        total += n;
    }
    return total;
}


/**
 * @brief Receives a length-prefixed message from a client
 * 
 * @param client_socket Socket to receive from
 * @param out_message Output parameter for the received message
 * @return true if message received successfully, false on error or disconnect
 */
bool receive_message(SOCKET client_socket, std::string& out_message) {
    // get the 4-byte length prefix
    uint32_t length_network = 0;
    if (recv_all(client_socket, reinterpret_cast<char*>(&length_network), sizeof(length_network)) <= 0) {
        return false;
    }
    
    const uint32_t message_length = ntohl(length_network);
    
    if (message_length == 0 || message_length > 1024) {
        std::cerr << "Invalid message length: " << message_length << "\n";
        return false;
    }
    
    // Receive the message payload
    out_message.resize(message_length);
    const int bytes_received = recv_all(client_socket, out_message.data(), message_length);
    
    return bytes_received == static_cast<int>(message_length);
}



/**
 * @brief Sends exactly the specified number of bytes to a socket
 * 
 * Handles partial sends by looping until all data is sent or an error occurs.
 * 
 * @param sock Socket to send to
 * @param buf Buffer containing data to send
 * @param len Number of bytes to send
 * @return Number of bytes sent, -1 on error
 */
int send_all(SOCKET sock, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n == SOCKET_ERROR) {
            return -1; // error
        }
        if (n == 0) {
            break; // connection closed
        }
        total += n;
    }

    return total;
}             

/**
 * @brief Broadcasts a message to all connected clients except the sender
 * 
 * Automatically removes clients that fail to receive the message.
 * 
 * @param sock Socket of the client who sent the message (excluded from broadcast)
 * @param message Buffer containing the complete message to send
 * @param message_length Length of the message buffer
 */
void broadcast_message(SOCKET sock, char* message_to_send, int len) {
    // send message to all clients on the list
    {
        std::shared_lock<std::shared_mutex> read_lock(client_mutex);
        for (const std::shared_ptr<client_info>& c : clients_list) {
            if (c->socket != sock) {
                int result = send_all(c->socket, message_to_send, len);
                if (result < 0 || result != len) {
                     std::cout << "Error sending message to " << c->socket << "\n"; 
                }
            }
        }
    }
}

/**
 * @brief Receives a length-prefixed message from a client
 * @param client client info struct, including the client username, and socket to connect to
 */
void handle_client(std::shared_ptr<client_info> client) {

    // Send welcome message to all clients
    std::string welcome_message = "\n----- " + client->name + " has joined the chat. -----" + "\n";
    std::vector<char> welcome_message_to_send = build_message_buffer(welcome_message);

    broadcast_message(client->socket, welcome_message_to_send.data(), welcome_message_to_send.size());

    std::cout << "[JOIN] " << client->name << " connected" << "\n";

    // constantly listen for oncoming messages from the client
    std::string incoming_message;
    while (running) {
        if (!receive_message(client->socket, incoming_message)) {
            // notify other clients on exit
            std::string exit_message = "\n----- " + client->name + " has left the chat. -----" + "\n";
            std::vector<char> exit_message_to_send = build_message_buffer(exit_message);

            broadcast_message(client->socket, exit_message_to_send.data(), exit_message_to_send.size());

            break;
        };

        // broadcast message to other clients
        std::string outgoing_message = client->name + ": " + incoming_message;
        std::vector<char> message_to_send = build_message_buffer(outgoing_message);

        broadcast_message(client->socket, message_to_send.data(), message_to_send.size());
    }

    std::cout << "[LEAVE] " << client->name << " disconnected" << "\n";

    // Clean up
    {
        std::unique_lock<std::shared_mutex> write_lock(client_mutex);
         auto it = std::find_if(clients_list.begin(), clients_list.end(), 
                [&](const std::shared_ptr<client_info>& c) {
                    return (c->socket == client->socket);
                }      
            );
        
        if (it != clients_list.end()) {
            clients_list.erase(it);
        }

    }
    
    shutdown(client->socket, SD_BOTH);
    closesocket(client->socket);  
}

/**
 * @brief Creates a listening socket + binds ip and port 
 * @param port Port to connect to, inputted by the user
 */
SOCKET create_listening_socket(const char* port) {
    addrinfo hints{};

    ZeroMemory(&hints, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    
    addrinfo* address_info = nullptr;
    const int result = getaddrinfo(nullptr, port, &hints, &address_info);
    
    if (result != 0) {
        std::cerr << "getaddrinfo failed: " << result << "\n";
        return INVALID_SOCKET;
    }
    
    // Use RAII-style cleanup
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_guard(address_info, freeaddrinfo);
    
    // Create socket
    SOCKET listen_socket = socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);
    
    if (listen_socket == INVALID_SOCKET) {
        std::cerr << "socket creation failed\n";
        return INVALID_SOCKET;
    }
    
    // Bind socket
    if (bind(listen_socket, address_info->ai_addr, static_cast<int>(address_info->ai_addrlen)) == SOCKET_ERROR) {
        std::cerr << "bind failed\n";
        closesocket(listen_socket);
        return INVALID_SOCKET;
    }
    
    // Listen for connections
    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen failed\n";
        closesocket(listen_socket);
        return INVALID_SOCKET;
    }
    
    return listen_socket;
}

// signal handler
void signal_handler(int) {
    running = false;
}



int main() {
#if defined(_WIN32)
    WSADATA wsa_data;
    int i_result;
    i_result = WSAStartup(MAKEWORD(2,2), &wsa_data);
    if (i_result != 0) {
        printf("WSAStartup failed: %d\n", i_result);
        return 1;                        
    }
#endif
    
    std::cout << "Chat Server v1.0\n";
    std::cout << "Initializing...\n";

    const SOCKET listen_socket = create_listening_socket(DEFAULT_PORT);
 
    if (listen_socket == INVALID_SOCKET) {
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }

#if defined(_WIN32)
    u_long mode = 1;  
    ioctlsocket(listen_socket, FIONBIO, &mode);
#else
    int flags = fcntl(listen_socket, F_GETFL, 0);
    fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    std::cout << "Server listening on port " << DEFAULT_PORT << "\n";
    std::cout << "Waiting for connections...\n";

    signal(SIGINT, signal_handler);

    while (running) {
        SOCKET client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) {
#if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
#else
            if (errno == EWOULDBLOCK || errno == EAGAIN)
#endif
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            std::cerr << "accept failed\n";
            continue;
        }

#if defined(_WIN32)
        u_long blocking = 0;
        ioctlsocket(client_socket, FIONBIO, &blocking);
#else
        int fl = fcntl(client_socket, F_GETFL, 0);
        fcntl(client_socket, F_SETFL, fl & ~O_NONBLOCK);
#endif

        uint32_t len_net;
        if (recv_all(client_socket, reinterpret_cast<char*>(&len_net), sizeof(len_net)) <= 0) {
            closesocket(client_socket);
            continue;
        };
        
        uint32_t name_len = ntohl(len_net);
        if (name_len <= 0 || name_len > 20) { 
            closesocket(client_socket);
            continue;
        }
        std::string name(name_len, '\0');

        if (recv_all(client_socket, &name[0], name_len) <= 0) {
            closesocket(client_socket);
            continue;
        };

        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        getpeername(client_socket, (sockaddr*)&client_addr, &addr_len);              

        char client_ip[INET_ADDRSTRLEN]; 
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        printf("Accepted Connection from: %s\n", client_ip);

        {
             std::unique_lock<std::shared_mutex> write_lock(client_mutex);

            if (clients_list.size() < 10) {
                auto new_client = std::make_shared<client_info>(client_info{
                        client_socket,
                        name
                    });

                clients_list.push_back(new_client);
                std::thread t(handle_client, new_client);
                t.detach();
            }  else {
                std::cerr << "Server full, rejecting client: " << name << "\n";
                closesocket(client_socket);
            }
        }
    }

    std::cout << "Server shutting down...\n";

    std::this_thread::sleep_for(std::chrono::seconds(1));

    {
        std::unique_lock<std::shared_mutex> lock(client_mutex);
        for (auto& client : clients_list) {
            shutdown(client->socket, SD_BOTH);
            closesocket(client->socket);
        }
        clients_list.clear();
    }

    shutdown(listen_socket, SD_BOTH);
    closesocket(listen_socket);
#if defined(_WIN32)
    WSACleanup();
#endif

    std::cout << "Server stopped.\n";
    
    return 0;
}
