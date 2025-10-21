#define _WIN32_WINNT 0x0600  

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <conio.h>

#define DEFAULT_PORT "3000"
#pragma comment(lib, "Ws2_32.lib")

std::atomic<bool> running{true};
std::mutex input_mutex;
std::atomic<bool> shutdown_called{false};

/**
 * @brief Safely shutdown a socket connection
 * @param sock The socket to shutdown
 * 
 * uses an atomic flag to ensure shutdown is called only once,
 * preventing multiple threads from closing the same socket simultaneously.
 *
 */
void safe_shutdown(SOCKET sock) {
    bool expected = false;
    if (shutdown_called.compare_exchange_strong(expected, true)) {
        shutdown(sock, SD_BOTH);
        closesocket(sock);
    }
}


/**
 * @brief Attempt connection to resolved addresses with ipv4/ipv6 fallback
 * @param result Linked list of resolved addresses from getaddrinfo
 * @return Connected socket or INVALID_SOCKET on failure
 * 
 * Iterates through all resolved addresses and attempts to connect to each.
 */
SOCKET connect_to_addresses(addrinfo* result) {
    SOCKET s = INVALID_SOCKET;
    for (auto ptr = result; ptr; ptr = ptr->ai_next) {
        if (ptr->ai_family != AF_INET && ptr->ai_family != AF_INET6) continue;

        SOCKET cand = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (cand == INVALID_SOCKET) continue;

        char str_buf[INET6_ADDRSTRLEN] = {0};
        const void* addr = nullptr;

        if (ptr->ai_family == AF_INET) {
            if (ptr->ai_addrlen < sizeof(sockaddr_in)) { closesocket(cand); continue; }
            auto* ipv4 = reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
            addr = &ipv4->sin_addr;
        } else { // AF_INET6
            if (ptr->ai_addrlen < sizeof(sockaddr_in6)) { closesocket(cand); continue; }
            auto* ipv6 = reinterpret_cast<sockaddr_in6*>(ptr->ai_addr);
            addr = &ipv6->sin6_addr;
        }

        if (!inet_ntop(ptr->ai_family, addr, str_buf, INET6_ADDRSTRLEN)) {
            closesocket(cand);
            continue;
        }

        if (connect(cand, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR) {
            closesocket(cand);
            continue;
        }

        std::cout << "connected to " << str_buf << std::endl;
        s = cand;
        break;
    }
    freeaddrinfo(result);
    return s;
}


/**
 * @brief Prompt user for username with validation
 * @return User-entered username (1-20 printable ASCII characters)
 */
std::string get_username() {
    std::string username;
    std::cout << "Enter a username: " << std::flush;

    while (true) {
        int ch = _getch();
        // if pressed enter, break
        if (ch == '\r') {
            if (!username.empty()) {
                std::cout << std::endl;
                break;
            }
            std::cout << "\33[2K\r" << "Username cannot be empty." << std::endl;
            std::cout << "Enter a username: " << std::flush;
        }
        // if backspace, backspace
        else if (ch == 8) { 
            if (!username.empty()) {
                username.pop_back();
                std::cout << "\b \b" << std::flush;
            }
        } 
        // else, add to the user input
        else if (username.size() < 20 && ch > 31 && ch < 128){
            username.push_back(ch);
            std::cout << (char)ch << std::flush;
        }
    }

    return username;
}


/**
 * @brief Build length-prefixed message buffer
 * @param payload Message payload to send
 * @return Buffer containing 4-byte length header + payload
 * 
 * Message format: [4 bytes: length in network byte order][payload]
 */
inline std::vector<char> build_message_buffer(const std::string& payload) {
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::vector<char> buffer(sizeof(len) + payload.size());

    memcpy(buffer.data(), &len, sizeof(len));
    memcpy(buffer.data() + sizeof(len), payload.data(), payload.size());

    return buffer;
}


/**
 * @brief Send all bytes in buffer, handling partial sends
 * @param sock Socket to send on
 * @param buf Data to send
 * @param len Number of bytes to send
 * @return Total bytes sent, or <= 0 on error/close
 * 
 */
int send_all(SOCKET sock, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) {
            return (total == 0) ? n : total; 
        }

        total += n;
    }

    return total;
}

/**
 * @brief Handle user input and message sending
 * @param sock Socket connected to server
 * @param username Current user's username
 * @param current_input Shared string for current input line
 * 
 * Manages keyboard input, processes exit commands,
 * and sends messages to the server
 */
void handle_user_input(SOCKET sock, std::string& username, std::string& current_input) {
        std::cout << username << ": " << std::flush;
        while (running) {
            // Read from user
            int ch = _getch(); 
            
            // if pressed enter, send current_input
            if (ch == '\r') {
                std::string message_copy;
                {
                    std::lock_guard<std::mutex> write_lock(input_mutex);
                    if (current_input.empty()) continue;
                    message_copy = current_input;
                    current_input.clear();
                }
                // handle quit command
                if (message_copy == "/quit") {
                    safe_shutdown(sock);
                    running = false;       
                    break;
                }

                // send message
                std::vector<char> message_to_send = build_message_buffer(message_copy); 
                int bytes_sent = send_all(sock, message_to_send.data(), message_to_send.size());

                if (bytes_sent != static_cast<int>(message_to_send.size())) {
                    printf("Error sending message: %d\n", WSAGetLastError());
                    safe_shutdown(sock);
                    running = false;
                    break; 
                }

                // display new input prompt
                {
                    std::lock_guard<std::mutex> write_lock(input_mutex);
                    std::cout << std::endl;
                    std::cout << username << ": " << std::flush;
                }
            }

            // if backspace, backspace
            else if (ch == 8) { 
                std::lock_guard<std::mutex> write_lock(input_mutex);
                if (!current_input.empty()) {
                    current_input.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
            } 
            // else, add to the user input (printable chars)
            else if (current_input.size() < 1024 && ch > 31 && ch < 128) {
                {
                    std::lock_guard<std::mutex> write_lock(input_mutex);
                    current_input.push_back(ch);
                    std::cout << (char)ch << std::flush;
                }
            }
        }
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
        if (n <= 0) {
            return (total == 0) ? n : total; 
        }
        total += n;
    }
    return total;
}

/**
 * @brief Handle incoming messages from server
 * @param sock Socket connected to server
 * @param username Current user's username
 * @param current_input Shared string for current input line
 * 
 * Continuously receives messages from the server and displays them
 * while preserving the current input prompt. 
 */
void handle_recv(SOCKET sock, std::string& username, std::string& current_input ) {
        while (running) {

            // read message length header
            uint32_t len_net;
            if (recv_all(sock, reinterpret_cast<char*>(&len_net), sizeof(len_net)) <= 0) {
                safe_shutdown(sock);
                running = false;
                break;
            }

            uint32_t len = ntohl(len_net); // convert from network to host byte order

            if (len == 0 || len > 1024) {
                printf("Warning: server sent invalid message size (%u bytes).\n", len);
                safe_shutdown(sock);
                running = false;
                break;
            }
            
            // read message payload
            std::string message(len, '\0');
            if (recv_all(sock, message.data(), len) <= 0) {
                safe_shutdown(sock);
                running = false;
                break;
            }

            //display message while preserving current input line
            {
                std::lock_guard<std::mutex> read_lock(input_mutex);
                std::string current_input_copy = current_input; 

                // clear line, display message, restore input prompt
                std::cout << "\33[2K\r" << message << std::endl
                    << username << ": " << current_input_copy << std::flush;
            }
        }
    }



int main(int argc, char* argv[]) {


    std::cout << "yo" << std::endl;
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server_address>" << std::endl;
        std::cerr << "Example: " << argv[0] << " 127.0.0.1" << std::endl;
        std::cerr << "Example: " << argv[0] << " example.com" << std::endl;
        return 1;
    }

    // get username
    std::string username = get_username();


    // Initialize Winsock
    WSADATA wsa_data;

    int i_result;

    i_result = WSAStartup(MAKEWORD(2,2), &wsa_data);
    if (i_result != 0) {
        printf("WSAStartup failed: %d\n", i_result);
        return 1;  
    }

    struct addrinfo *result = NULL, hints;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    // Resolve the server address and port
    i_result = getaddrinfo(argv[1], DEFAULT_PORT, &hints, &result);
    if (i_result != 0) {
        printf("getaddrinfo failed: %d\n", i_result);
        WSACleanup();
        return 1;
    }

    // Attempt to connect to the first address returned by
    // the call to getaddrinfo
    SOCKET connect_socket = connect_to_addresses(result);

    if (connect_socket == INVALID_SOCKET) {
        printf("Unable to connect to server!\n");
        WSACleanup();
        return 1;
    }          

    std::vector<char> username_to_send = build_message_buffer(username);
    i_result = send_all(connect_socket, username_to_send.data(), username_to_send.size());

    if (i_result != (int)username_to_send.size()) {  
        printf("Error setting name (sent %d/%zu bytes)\n", i_result, username_to_send.size());
        closesocket(connect_socket);
        WSACleanup();
        return 1;
    }

    std::string current_input;

    std::thread input_thread(handle_user_input, connect_socket, std::ref(username), std::ref(current_input));
 
    std::thread receiver_thread(handle_recv, connect_socket, std::ref(username), std::ref(current_input));
    
    input_thread.join();
    receiver_thread.join();

    std::cout << std::endl;
    std::cout << "Quitting..." << std::endl;

    WSACleanup();
    return 0;
}
