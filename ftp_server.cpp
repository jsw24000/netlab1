#include "protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

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
    if (inet_ntop(AF_INET,
                  &client_addr.sin_addr,
                  ip_buffer,
                  sizeof(ip_buffer)) == nullptr) {
        std::cerr << "accept succeeded, but failed to format client IP: "
                  << std::strerror(errno) << std::endl;
        return;
    }

    std::cout << "TCP client connected from " << ip_buffer << ':'
              << ntohs(client_addr.sin_port) << std::endl;
}

bool send_open_reply(int client_fd) {
    const myftp::Header reply = myftp::make_header(
        myftp::MessageType::OPEN_CONN_REPLY,
        1,
        myftp::kHeaderLength);
    return myftp::send_all(client_fd, &reply, sizeof(reply));
    // 调用 send_all 函数将 OPEN_CONN_REPLY 消息发送给客户端，表示 myFTP 连接已建立
}

void handle_client(int client_fd) {
    bool myftp_open = false;

    while (true) {
        myftp::Header header{};
        if (!myftp::recv_all(client_fd, &header, sizeof(header))) {
            std::cout << "TCP client disconnected" << std::endl;
            return;
        }

        if (!myftp::has_valid_magic(header)) {
            std::cerr << "invalid myFTP magic number" << std::endl;
            return;
        }

        const std::uint32_t message_length = myftp::total_length(header);
        if (message_length < myftp::kHeaderLength) {
            std::cerr << "invalid myFTP message length" << std::endl;
            return;
        }

        const std::uint32_t payload_length =
            message_length - myftp::kHeaderLength;
        // 读取消息头后，计算消息的总长度和有效负载长度。消息总长度必须大于等于 myFTP 协议头的长度（12 字节），否则视为无效消息。有效负载长度为总长度减去协议头长度。

        // Stage 2 only needs OPEN, but consume any payload of an unsupported
        // message so that the TCP byte stream stays aligned at the next header.
        std::vector<std::uint8_t> payload(payload_length);
        if (payload_length > 0 &&
            !myftp::recv_all(client_fd, payload.data(), payload.size())) {
            std::cerr << "failed to receive myFTP payload" << std::endl;
            return;
        }

        const auto type = static_cast<myftp::MessageType>(header.m_type);

        if (type == myftp::MessageType::OPEN_CONN_REQUEST) {
            if (message_length != myftp::kHeaderLength) {
                // OPEN_CONN_REQUEST 消息不应包含有效负载，因此总长度必须等于协议头长度（12 字节）。如果不满足此条件，则视为无效消息。
                std::cerr << "invalid OPEN_CONN_REQUEST length" << std::endl;
                return;
            }

            if (!send_open_reply(client_fd)) {
                std::cerr << "failed to send OPEN_CONN_REPLY" << std::endl;
                return;
            }

            myftp_open = true; // 收到了 OPEN_CONN_REQUEST 并成功发送了 OPEN_CONN_REPLY，表示 myFTP 连接已建立
            std::cout << "myFTP OPEN handshake completed" << std::endl;
            continue;
        }

        if (!myftp_open) {
            std::cerr << "received myFTP command before OPEN handshake"
                      << std::endl;
            return;
        }

        // LIST/CD/GET/PUT/SHA/QUIT will be implemented in later stages.
        std::cerr << "myFTP message type 0x" << std::hex
                  << static_cast<int>(header.m_type) << std::dec
                  << " is not implemented yet" << std::endl;
    }
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

    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0); // 创建一个 TCP 套接字，用于监听客户端连接请求
    if (listen_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << std::endl;
        return 1;
    }

    int reuse = 1;
    if (setsockopt(listen_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed: "
                  << std::strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }
    //

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

    if (listen(listen_fd, SOMAXCONN) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    std::cout << "TCP server listening on " << listen_ip << ':' << listen_port
              << std::endl;

    // Still single-client-at-a-time at this stage. Concurrency will be added
    // after the core myFTP request/reply behavior is correct.
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);

        int client_fd = -1;
        while (true) {
            client_fd = accept(listen_fd,
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
        handle_client(client_fd);
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
