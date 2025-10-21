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

#pragma comment(lib, "Ws2_32.lib")

namespace config {
    constexpr const char* DEFAULT_PORT = "3000";
    constexpr int BACKLOG_SIZE = SOMAXCONN;
    constexpr size_t MAX_MESSAGE_SIZE = 1024; // safety limit
}


struct client_info {
    SOCKET socket;
    std::string name;
};

std::vector<client_info> clients_list;
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
 * @brief Receives exactly the specified number of bytes from a socket
 * 
 * Handles partial receives by looping until all data is received or an error occurs.
 * 
 * @param sock Socket to receive from
 * @param buf Buffer to store received data
 * @param len Number of bytes to receive
 * @return Number of bytes received, or <=0 on error/close.
 *         
 *         
 */
int recv_all(SOCKET sock, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) {
            return (total == 0) ? n : total; 
        }
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
    
    if (message_length == 0 || message_length > config::MAX_MESSAGE_SIZE) {
        std::cerr << "Invalid message length: " << message_length << std::endl;
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
 * @param output_sock Socket of the client who sent the message (excluded from broadcast)
 * @param message_to_send Buffer containing the complete message to send
 * @param len Length of the message buffer
 */
void broadcast_message(SOCKET output_sock, char* message_to_send, int len) {
    std::vector<SOCKET> disconnected_clients;

    // send message to all clients on the list
    {
        std::shared_lock<std::shared_mutex> read_lock(client_mutex);
        for (client_info& c : clients_list) {
            if (c.socket != output_sock) {
                int result = send_all(c.socket, message_to_send, len);

                // if socket error, add client on list to remove
                if (result == SOCKET_ERROR) {
                    disconnected_clients.push_back(c.socket);
                } 
            }
        }
    }

    // remove all disconnected clients 
    if (!disconnected_clients.empty()) {
        std::unique_lock<std::shared_mutex> write_lock(client_mutex);
        for (SOCKET client : disconnected_clients) {

            shutdown(client, SD_BOTH);
            closesocket(client);

            clients_list.erase(
                std::remove_if(clients_list.begin(), clients_list.end(),
                    [&](const client_info& c){ return c.socket == client; }),
                clients_list.end()
            );
        }
    }
}

/**
 * @brief Receives a length-prefixed message from a client
 * @param client client info struct, including the client username, and socket to connect to
 */
void handle_client(client_info client) {

    // Send welcome message to all clients
    std::string welcome_message = client.name + " has joined the chat.";
    std::vector<char> welcome_message_to_send = build_message_buffer(welcome_message);

    broadcast_message(client.socket, welcome_message_to_send.data(), welcome_message_to_send.size());

    std::cout << "[JOIN] " << client.name << " connected" << std::endl;

    // constantly listen for oncoming messages from the client
    std::string incoming_message;
    while (running) {
        if (!receive_message(client.socket, incoming_message)) break;

        // broadcast message to other clients
        std::string outgoing_message = client.name + ": " + incoming_message;
        std::vector<char> message_to_send = build_message_buffer(outgoing_message);

        broadcast_message(client.socket, message_to_send.data(), message_to_send.size());
    }

    // notify other clients on exit
    std::string exit_message = client.name + " has left the chat.";
    std::vector<char> exit_message_to_send = build_message_buffer(exit_message);

    broadcast_message(client.socket, exit_message_to_send.data(), exit_message_to_send.size());

    std::cout << "[LEAVE] " << client.name << " disconnected" << std::endl;

    // Clean up
    {
        std::unique_lock<std::shared_mutex> write_lock(client_mutex);
        clients_list.erase(
            std::remove_if(clients_list.begin(), clients_list.end(),
                [&](const client_info& c){ return c.socket == client.socket; }),
            clients_list.end()
        );
    }
    
    shutdown(client.socket, SD_BOTH);
    closesocket(client.socket);  
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
        std::cerr << "getaddrinfo failed: " << result << std::endl;
        return INVALID_SOCKET;
    }
    
    // Use RAII-style cleanup
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_guard(address_info, freeaddrinfo);
    
    // Create socket
    SOCKET listen_socket = socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);
    
    if (listen_socket == INVALID_SOCKET) {
        std::cerr << "socket creation failed: " << WSAGetLastError() << std::endl;
        return INVALID_SOCKET;
    }
    
    // Bind socket
    if (bind(listen_socket, address_info->ai_addr, static_cast<int>(address_info->ai_addrlen)) == SOCKET_ERROR) {
        std::cerr << "bind failed: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket);
        return INVALID_SOCKET;
    }
    
    // Listen for connections
    if (listen(listen_socket, config::BACKLOG_SIZE) == SOCKET_ERROR) {
        std::cerr << "listen failed: " << WSAGetLastError() << std::endl;
        closesocket(listen_socket);
        return INVALID_SOCKET;
    }
    
    return listen_socket;
}

int main() {
    WSADATA wsa_data;

    int i_result;
    
    std::cout << "yo" << std::endl;

    // Initialize Winsock
    i_result = WSAStartup(MAKEWORD(2,2), &wsa_data);

    if (i_result != 0) {
        printf("WSAStartup failed: %d\n", i_result);
        return 1;                        
    }
    
    std::cout << "Chat Server v1.0" << std::endl;
    std::cout << "Initializing..." << std::endl;

    // create listening socket
    const SOCKET listen_socket = create_listening_socket(config::DEFAULT_PORT);

    if (listen_socket == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }
    

    std::cout << "Server listening on port " << config::DEFAULT_PORT << std::endl;
    std::cout << "Waiting for connections..." << std::endl;

    // constantly listen for a connection
    while (running) {

        // Accept a client socket
        SOCKET client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) {
            printf("accept failed: %d\n", WSAGetLastError());
            continue;
        }

        // Receiving the client's name:
        // grab length of name
        int32_t len_net;
        if (recv_all(client_socket, reinterpret_cast<char*>(&len_net), sizeof(len_net)) <= 0) {
            printf("Error receiving message name: 1\n", WSAGetLastError());
            closesocket(client_socket);
            continue;
        };
        
        // use length to build name buffer
        int32_t name_len = ntohl(len_net);
        std::string name(name_len, '\0');

        if (recv_all(client_socket, &name[0], name_len) <= 0) {
            printf("Error receiving message name: 2\n", WSAGetLastError());
            closesocket(client_socket);
            continue;
        };

        // get client IP and log in terminal
        sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        getpeername(client_socket, (sockaddr*)&client_addr, &addr_len);

        char client_ip[INET_ADDRSTRLEN]; // 16 bytes
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        printf("Accepted Connection from: %s\n", client_ip);

        // add new client to the list and create a thread for it
        {
            std::unique_lock<std::shared_mutex> write_lock(client_mutex);

            client_info new_client = {
                client_socket,
                name
            };

            clients_list.push_back(new_client);
            std::thread t(handle_client, new_client);
            t.detach();
        }
    }
   
    // clean up
    shutdown(listen_socket, SD_BOTH);
    closesocket(listen_socket);
    WSACleanup();

    std::cout << "yo" << std::endl;
    
    return 0;
}
