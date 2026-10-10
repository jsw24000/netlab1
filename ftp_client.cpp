#include "protocol.hpp"

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
    // Transport layer: create an IPv4 TCP socket.
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << std::endl;
        return -1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "invalid IPv4 address: " << server_ip << std::endl;
        close(socket_fd);
        return -1;
    }

    // connect() establishes only the TCP connection.
    if (connect(socket_fd,
                reinterpret_cast<const sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << std::endl;
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

bool perform_open_handshake(int socket_fd) {
    // Application layer: after TCP connect() succeeds, myFTP still requires an
    // OPEN_CONN_REQUEST / OPEN_CONN_REPLY exchange.
    const myftp::Header request = myftp::make_header(
        myftp::MessageType::OPEN_CONN_REQUEST,
        0,
        myftp::kHeaderLength);

    if (!myftp::send_all(socket_fd, &request, sizeof(request))) {
        std::cerr << "failed to send OPEN_CONN_REQUEST" << std::endl;
        return false;
    }

    myftp::Header reply{};
    if (!myftp::recv_all(socket_fd, &reply, sizeof(reply))) {
        std::cerr << "failed to receive OPEN_CONN_REPLY" << std::endl;
        return false;
    }

    // Validate the reply message.
    if (!myftp::has_valid_magic(reply) ||
        reply.m_type != static_cast<std::uint8_t>(
                            myftp::MessageType::OPEN_CONN_REPLY) ||
        reply.m_status != 1 ||
        myftp::total_length(reply) != myftp::kHeaderLength) {
        std::cerr << "invalid OPEN_CONN_REPLY" << std::endl;
        return false;
    }

    return true;
}

bool perform_quit_handshake(int socket_fd) {
    const myftp::Header request = myftp::make_header(
        myftp::MessageType::QUIT_REQUEST,
        0,
        myftp::kHeaderLength);

    // 发送 QUIT_REQUEST 消息给服务器，表示客户端希望关闭 myFTP 连接
    if (!myftp::send_all(socket_fd, &request, sizeof(request))) {
        std::cerr << "failed to send QUIT_REQUEST" << std::endl;
        return false;
    }

    myftp::Header reply{};
    if (!myftp::recv_all(socket_fd, &reply, sizeof(reply))) {
        std::cerr << "failed to receive QUIT_REPLY" << std::endl;
        return false;
    }

    if (!myftp::has_valid_magic(reply) ||
        reply.m_type != static_cast<std::uint8_t>(
                            myftp::MessageType::QUIT_REPLY) ||
        myftp::total_length(reply) != myftp::kHeaderLength) {
        std::cerr << "invalid QUIT_REPLY" << std::endl;
        return false;
    }

    // 如果 QUIT_REPLY 消息的魔数、消息类型或总长度不符合 myFTP 协议规范，则视为无效消息，返回 false。否则，返回 true，表示 myFTP 连接已成功关闭
    return true;
}

}  // namespace

int main() {
    // socket_fd < 0 means the client is in the Idle state.
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
                std::cerr << "a myFTP connection is already open" << std::endl;
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

            const int new_socket_fd = connect_to_server(server_ip, server_port);
            if (new_socket_fd < 0) {
                continue;
            }

            if (!perform_open_handshake(new_socket_fd)) {
                close(new_socket_fd);
                continue;
            }

            socket_fd = new_socket_fd;
            std::cout << "myFTP connection established to " << server_ip << ':'
                      << server_port << std::endl;
            continue;
        }

        if (command == "quit") {
            if (socket_fd >= 0) {
                // In the Main state, quit performs the myFTP close handshake
                // and then returns the client to the Idle state.
                const bool quit_ok = perform_quit_handshake(socket_fd);
                close(socket_fd);
                socket_fd = -1;

                if (quit_ok) {
                    std::cout << "myFTP connection closed" << std::endl;
                }
            } else {
                // In the Idle state, quit exits the client process.
                return 0;
            }
            continue;
        }

        std::cerr << "command not implemented yet: " << command << std::endl;
    }

    if (socket_fd >= 0) {
        close(socket_fd);
    }

    return 0;
}
