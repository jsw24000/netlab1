#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool parse_port(const std::string& text, uint16_t& port) {
    try {
        std::size_t pos = 0;
        const int value = std::stoi(text, &pos);
        if (pos != text.size() || value < 1 || value > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

int connect_to_server(const std::string& server_ip, uint16_t server_port) {
    // 1. Create an IPv4 TCP socket.
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << std::endl;
        return -1;
    }

    // 2. Build the server's IPv4 socket address.
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "invalid IPv4 address: " << server_ip << std::endl;
        close(socket_fd);
        return -1;
    }

    // 3. Establish the TCP connection.
    if (connect(socket_fd,
                reinterpret_cast<const sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << std::endl;
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

}  // namespace

int main() {
    // The official client is started with no command-line arguments.
    // Stage 1 implements only the transport-level part of `open` and `quit`.
    int socket_fd = -1;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        input >> command;

        if (command.empty()) {
            continue;
        }

        if (command == "open") {
            if (socket_fd >= 0) {
                std::cerr << "a TCP connection is already open" << std::endl;
                continue;
            }

            std::string server_ip;
            std::string port_text;
            std::string extra;
            if (!(input >> server_ip >> port_text) || (input >> extra)) {
                std::cerr << "usage: open <IP> <port>" << std::endl;
                continue;
            }

            uint16_t server_port = 0;
            if (!parse_port(port_text, server_port)) {
                std::cerr << "invalid port: " << port_text << std::endl;
                continue;
            }

            socket_fd = connect_to_server(server_ip, server_port);
            if (socket_fd >= 0) {
                std::cout << "TCP connection established to " << server_ip << ':'
                          << server_port << std::endl;
                // The myFTP OPEN_CONN_REQUEST / OPEN_CONN_REPLY handshake is
                // intentionally NOT implemented in Stage 1.
            }
            continue;
        }

        if (command == "quit") {
            if (socket_fd >= 0) {
                close(socket_fd);
                socket_fd = -1;
                std::cout << "TCP connection closed" << std::endl;
            } else {
                // This matches the future client state-machine behavior:
                // quit while idle exits the client.
                return 0;
            }
            continue;
        }

        std::cerr << "command not implemented in TCP-only stage: " << command
                  << std::endl;
    }

    if (socket_fd >= 0) {
        close(socket_fd);
    }

    return 0;
}
