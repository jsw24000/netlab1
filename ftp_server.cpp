#include "protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

bool get_file_list(const std::filesystem::path& current_directory,
                   std::string& file_list) {
    // Run `ls` in a child shell so each client's logical working directory is
    // independent; do not call process-wide chdir() in the server.
    const std::string command =
        "cd " + shell_quote(current_directory.string()) + " && ls";

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }

    char buffer[1024];
    while (true) {
        const std::size_t count = std::fread(buffer, 1, sizeof(buffer), pipe);
        if (count > 0) {
            file_list.append(buffer, count);
        }

        if (count < sizeof(buffer)) {
            if (std::feof(pipe)) {
                break;
            }
            if (std::ferror(pipe)) {
                pclose(pipe);
                return false;
            }
        }
    }

    return pclose(pipe) == 0;
}

bool send_list_reply(int client_fd, const std::string& file_list) {
    // The LIST_REPLY payload is a null-terminated string, so the terminating
    // '\0' is included in m_length and must also be sent on the TCP stream.
    const std::uint32_t payload_length =
        static_cast<std::uint32_t>(file_list.size() + 1);
    const myftp::Header reply = myftp::make_header(
        myftp::MessageType::LIST_REPLY,
        0,
        myftp::kHeaderLength + payload_length);

    if (!myftp::send_all(client_fd, &reply, sizeof(reply))) {
        return false;
    }

    return myftp::send_all(client_fd,
                           file_list.c_str(),
                           payload_length);
}

bool send_change_dir_reply(int client_fd, bool success) {
    const myftp::Header reply = myftp::make_header(
        myftp::MessageType::CHANGE_DIR_REPLY,
        success ? 1 : 0,
        myftp::kHeaderLength);
    return myftp::send_all(client_fd, &reply, sizeof(reply));
}

bool send_get_reply(int client_fd, bool success) {
    const myftp::Header reply = myftp::make_header(
        myftp::MessageType::GET_REPLY,
        success ? 1 : 0,
        myftp::kHeaderLength);
    return myftp::send_all(client_fd, &reply, sizeof(reply));
}

bool send_file_data(int client_fd, const std::filesystem::path& file_path) {
    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(file_path, ec);
    if (ec ||
        file_size >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::uint32_t>::max() -
                myftp::kHeaderLength)) {
        return false;
    }

    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        return false;
    }

    const myftp::Header data_header = myftp::make_header(
        myftp::MessageType::FILE_DATA,
        0,
        myftp::kHeaderLength + static_cast<std::uint32_t>(file_size));

    // 第一次返回，返回一个 FILE_DATA 消息头，告诉客户端接下来要发送的文件数据的总长度（包括协议头和有效负载）。客户端收到这个消息头后，就知道要接收多少字节的数据。
    if (!myftp::send_all(client_fd, &data_header, sizeof(data_header))) {
        return false;
    }

    // Stream the file in chunks rather than assuming one send() can transmit
    // the whole file. This is binary-safe and also works for the large-file
    // tests.
    // 第二次正式返回，返回文件数据的有效负载部分。为了避免一次性发送整个文件数据，使用一个缓冲区分块读取文件数据，并调用 send_all 函数将每一块数据发送给客户端。这样可以保证二进制安全，并且适用于大文件传输。
    char buffer[8192];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0 &&
            !myftp::send_all(client_fd,
                             buffer,
                             static_cast<std::size_t>(count))) {
            return false;
        }
    }

    return input.eof();
}

bool send_put_reply(int client_fd) {
    const myftp::Header reply = myftp::make_header(
        myftp::MessageType::PUT_REPLY,
        0,
        myftp::kHeaderLength);
    return myftp::send_all(client_fd, &reply, sizeof(reply));
}

bool receive_file_data(int client_fd,
                       const std::filesystem::path& file_path) {
    // After PUT_REPLY, the next myFTP message must be FILE_DATA.
    myftp::Header data_header{};

    // 先接收 FILE_DATA 消息头，获取文件数据的总长度（包括协议头和有效负载）。然后根据总长度计算有效负载长度，并创建一个输出文件流，将接收到的文件数据写入指定的文件路径。使用一个缓冲区分块接收文件数据，直到接收完毕。
    if (!myftp::recv_all(client_fd, &data_header, sizeof(data_header))) {
        return false;
    }

    if (!myftp::has_valid_magic(data_header) ||
        data_header.m_type != static_cast<std::uint8_t>(
                                  myftp::MessageType::FILE_DATA)) {
        return false;
    }

    const std::uint32_t data_message_length =
        myftp::total_length(data_header);
    if (data_message_length < myftp::kHeaderLength) {
        return false;
    }

    std::uint32_t remaining =
        data_message_length - myftp::kHeaderLength;

    // An existing file on the Server is overwritten, and binary mode preserves
    // the FILE_DATA payload byte-for-byte.
    std::ofstream output(file_path,
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    char buffer[8192];
    while (remaining > 0) {
        const std::size_t chunk_size =
            remaining < sizeof(buffer)
                ? static_cast<std::size_t>(remaining)
                : sizeof(buffer);

        if (!myftp::recv_all(client_fd, buffer, chunk_size)) {
            return false;
        }

        output.write(buffer, static_cast<std::streamsize>(chunk_size));
        if (!output) {
            return false;
        }

        remaining -= static_cast<std::uint32_t>(chunk_size);
    }

    return true;
}

bool send_quit_reply(int client_fd) {
    const myftp::Header reply = myftp::make_header(
        myftp::MessageType::QUIT_REPLY,
        0,
        myftp::kHeaderLength);
    return myftp::send_all(client_fd, &reply, sizeof(reply));
}

void handle_client(int client_fd) {
    bool myftp_open = false;

    // Each client maintains its own logical current directory. This starts at
    // the directory from which ftp_server itself was launched.
    std::filesystem::path current_directory = std::filesystem::current_path();

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

        const std::uint32_t message_length = myftp::total_length(header); // 可以直接作用于 myftp::Header 对象，获取消息的总长度（包括协议头和有效负载）。这个长度是从网络字节序转换为主机字节序后的值，用于确定接下来需要读取多少字节的数据。
        if (message_length < myftp::kHeaderLength) {
            std::cerr << "invalid myFTP message length" << std::endl;
            return;
        }

        const std::uint32_t payload_length =
            message_length - myftp::kHeaderLength;
        // 读取消息头后，计算消息的总长度和有效负载长度。消息总长度必须大于等于 myFTP 协议头的长度（12 字节），否则视为无效消息。有效负载长度为总长度减去协议头长度。

        // Consume the payload before dispatching the message so that the TCP
        // byte stream stays aligned at the next myFTP header.
        std::vector<std::uint8_t> payload(payload_length);
        if (payload_length > 0 &&
            !myftp::recv_all(client_fd, payload.data(), payload.size())) {
            std::cerr << "failed to receive myFTP payload" << std::endl; // 如果有效负载长度大于 0，则尝试接收有效负载数据。如果接收失败，则视为无效消息，返回 false。
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

        if (type == myftp::MessageType::LIST_REQUEST) {
            if (message_length != myftp::kHeaderLength) {
                std::cerr << "invalid LIST_REQUEST length" << std::endl;
                return;
            }

            std::string file_list;
            if (!get_file_list(current_directory, file_list)) {
                std::cerr << "failed to get file list" << std::endl;
                return;
            }

            if (!send_list_reply(client_fd, file_list)) {
                std::cerr << "failed to send LIST_REPLY" << std::endl;
                return;
            }

            continue;
        }

        if (type == myftp::MessageType::CHANGE_DIR_REQUEST) {
            if (payload.empty() || payload.back() != '\0') {
                std::cerr << "invalid CHANGE_DIR_REQUEST payload" << std::endl;
                return;
            }

            const std::string directory(
                reinterpret_cast<const char*>(payload.data()));

            // The payload must contain exactly one null-terminated string.
            if (directory.size() + 1 != payload.size()) {
                std::cerr << "invalid CHANGE_DIR_REQUEST payload" << std::endl;
                return;
            }

            const std::filesystem::path next_directory =
                current_directory / directory;

            std::error_code ec;
            const bool exists =
                std::filesystem::is_directory(next_directory, ec) && !ec;

            if (exists) {
                current_directory = next_directory.lexically_normal();
            }

            if (!send_change_dir_reply(client_fd, exists)) {
                std::cerr << "failed to send CHANGE_DIR_REPLY" << std::endl;
                return;
            }

            continue;
        }

        if (type == myftp::MessageType::GET_REQUEST) {
            if (payload.empty() || payload.back() != '\0') {
                std::cerr << "invalid GET_REQUEST payload" << std::endl;
                return;
            }

            const std::string filename(
                reinterpret_cast<const char*>(payload.data()));

            // The payload must contain exactly one null-terminated file name.
            if (filename.size() + 1 != payload.size()) {
                std::cerr << "invalid GET_REQUEST payload" << std::endl;
                return;
            }

            const std::filesystem::path file_path =
                current_directory / filename;

            std::error_code ec;
            const bool exists =
                std::filesystem::is_regular_file(file_path, ec) && !ec;

            if (!send_get_reply(client_fd, exists)) {
                std::cerr << "failed to send GET_REPLY" << std::endl;
                return;
            }

            if (exists && !send_file_data(client_fd, file_path)) {
                std::cerr << "failed to send FILE_DATA" << std::endl;
                return;
            }

            continue;
        }

        if (type == myftp::MessageType::PUT_REQUEST) {
            if (payload.empty() || payload.back() != '\0') {
                std::cerr << "invalid PUT_REQUEST payload" << std::endl;
                return;
            }

            const std::string filename(
                reinterpret_cast<const char*>(payload.data()));

            // The payload must contain exactly one null-terminated file name.
            if (filename.size() + 1 != payload.size()) {
                std::cerr << "invalid PUT_REQUEST payload" << std::endl;
                return;
            }

            const std::filesystem::path file_path =
                current_directory / filename;

            if (!send_put_reply(client_fd)) {
                std::cerr << "failed to send PUT_REPLY" << std::endl;
                return;
            }

            // The Client sends FILE_DATA immediately after receiving PUT_REPLY.
            if (!receive_file_data(client_fd, file_path)) {
                std::cerr << "failed to receive FILE_DATA" << std::endl;
                return;
            }

            continue;
        }

        if (type == myftp::MessageType::QUIT_REQUEST) {
            // QUIT_REQUEST 消息不应包含有效负载，因此总长度必须等于协议头长度（12 字节）。如果不满足此条件，则视为无效消息。
            if (message_length != myftp::kHeaderLength) {
                std::cerr << "invalid QUIT_REQUEST length" << std::endl;
                return;
            }

            if (!send_quit_reply(client_fd)) {
                std::cerr << "failed to send QUIT_REPLY" << std::endl;
                return;
            }

            std::cout << "myFTP QUIT handshake completed" << std::endl;
            return;
        }

        // SHA will be implemented in a later stage.
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
