#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
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

void print_client_address(const sockaddr_in& client_addr) {
    char ip_buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_buffer, sizeof(ip_buffer)) == nullptr) {
        std::cerr << "accept succeeded, but failed to format client IP: "
                  << std::strerror(errno) << std::endl;
        return;
    }

    std::cout << "TCP client connected from " << ip_buffer << ':'
              << ntohs(client_addr.sin_port) << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <IP> <port>" << std::endl;
        return 1;
    }

    const std::string listen_ip = argv[1];
    uint16_t listen_port = 0;
    if (!parse_port(argv[2], listen_port)) {
        std::cerr << "invalid port: " << argv[2] << std::endl;
        return 1;
    }

    // 1. Create an IPv4 TCP socket.
    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << std::endl;
        return 1;
    }

    // Allow quick restart of the server after it exits.
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed: " << std::strerror(errno)
                  << std::endl;
        close(listen_fd);
        return 1;
    }

    // 2. Build the local IPv4 address and bind it to the socket.
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(listen_port);

    if (inet_pton(AF_INET, listen_ip.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "invalid IPv4 address: " << listen_ip << std::endl;
        close(listen_fd);
        return 1;
    }

    if (bind(listen_fd,
             reinterpret_cast<const sockaddr*>(&server_addr),
             sizeof(server_addr)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    // 3. Turn the socket into a listening socket.
    if (listen(listen_fd, SOMAXCONN) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    std::cout << "TCP server listening on " << listen_ip << ':' << listen_port
              << std::endl;

    // Stage 1 only: accept one client at a time and wait until that TCP
    // connection closes. myFTP message parsing will be added in the next stage.
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);

        int client_fd = -1;
        while (true) {
            client_fd = accept(
                listen_fd,
                reinterpret_cast<sockaddr*>(&client_addr),
                &client_addr_len);

            if (client_fd >= 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "accept failed: " << std::strerror(errno) << std::endl;
            close(listen_fd);
            return 1;
        }

        print_client_address(client_addr);

        // There is no application-layer protocol in Stage 1 yet. recv() is used
        // only to detect whether the peer has closed the TCP connection.
        char buffer[1024];
        while (true) {
            const ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
            if (received > 0) {
                // Application data is intentionally ignored in Stage 1.
                continue;
            }
            if (received == 0) {
                std::cout << "TCP client disconnected" << std::endl;
                break;
            }
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "recv failed: " << std::strerror(errno) << std::endl;
            break;
        }

        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
