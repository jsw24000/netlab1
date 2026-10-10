#include "protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <sstream>
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

bool perform_list_request(int socket_fd) {
    const myftp::Header request = myftp::make_header(
        myftp::MessageType::LIST_REQUEST,
        0,
        myftp::kHeaderLength);

    if (!myftp::send_all(socket_fd, &request, sizeof(request))) {
        std::cerr << "failed to send LIST_REQUEST" << std::endl;
        return false;
    }

    myftp::Header reply{};
    if (!myftp::recv_all(socket_fd, &reply, sizeof(reply))) {
        std::cerr << "failed to receive LIST_REPLY" << std::endl;
        return false;
    }

    if (!myftp::has_valid_magic(reply) ||
        reply.m_type != static_cast<std::uint8_t>(
                            myftp::MessageType::LIST_REPLY)) {
        std::cerr << "invalid LIST_REPLY" << std::endl;
        return false;
    }

    // 如果成功接收到返回的协议头，则获取 LIST_REPLY 消息的总长度，并计算有效负载长度。有效负载长度必须大于等于 1 字节（至少包含一个空字符），否则视为无效消息，返回 false。                    
    const std::uint32_t message_length = myftp::total_length(reply); // 可以直接作用于 reply.m_length 字段，获取 LIST_REPLY 消息的总长度，包括协议头和有效负载
    if (message_length < myftp::kHeaderLength + 1) {
        std::cerr << "invalid LIST_REPLY length" << std::endl;
        return false;
    }

    const std::uint32_t payload_length =
        message_length - myftp::kHeaderLength;
    std::vector<std::uint8_t> payload(payload_length);

    // 下一步再接收 LIST_REPLY 消息的有效负载部分，即文件列表字符串。有效负载长度为总长度减去协议头长度，必须大于等于 1 字节（至少包含一个空字符）。如果接收失败，则返回 false。
    if (!myftp::recv_all(socket_fd, payload.data(), payload.size())) {
        std::cerr << "failed to receive LIST_REPLY payload" << std::endl;
        return false;
    }

    // LIST_REPLY payload is one null-terminated string.
    if (payload.back() != '\0') {
        std::cerr << "invalid LIST_REPLY payload" << std::endl;
        return false;
    }

    std::cout.write(reinterpret_cast<const char*>(payload.data()),
                    static_cast<std::streamsize>(payload.size() - 1));
    std::cout.flush();
    return true;
}

bool perform_change_dir_request(int socket_fd, const std::string& directory) {
    // CHANGE_DIR_REQUEST carries one null-terminated directory name as payload.
    const std::uint32_t payload_length =
        static_cast<std::uint32_t>(directory.size() + 1);
    const myftp::Header request = myftp::make_header(
        myftp::MessageType::CHANGE_DIR_REQUEST,
        0,
        myftp::kHeaderLength + payload_length);

    if (!myftp::send_all(socket_fd, &request, sizeof(request)) || // 前半部分的信息被 server 当作协议头去解析
        !myftp::send_all(socket_fd, directory.c_str(), payload_length)) { // 后半部分的有效负载被 server 当作目录名去解析
        std::cerr << "failed to send CHANGE_DIR_REQUEST" << std::endl;
        return false;
    }

    myftp::Header reply{};
    if (!myftp::recv_all(socket_fd, &reply, sizeof(reply))) {
        std::cerr << "failed to receive CHANGE_DIR_REPLY" << std::endl;
        return false;
    }

    if (!myftp::has_valid_magic(reply) ||
        reply.m_type != static_cast<std::uint8_t>(
                            myftp::MessageType::CHANGE_DIR_REPLY) ||
        (reply.m_status != 0 && reply.m_status != 1) ||
        myftp::total_length(reply) != myftp::kHeaderLength) {
        std::cerr << "invalid CHANGE_DIR_REPLY" << std::endl;
        return false;
    }

    if (reply.m_status == 0) {
        std::cerr << "server directory does not exist" << std::endl;
    }

    return true;
}

bool perform_get_request(int socket_fd, const std::string& filename) {
    // GET_REQUEST carries one null-terminated file name as payload.
    const std::uint32_t payload_length =
        static_cast<std::uint32_t>(filename.size() + 1);
    const myftp::Header request = myftp::make_header(
        myftp::MessageType::GET_REQUEST,
        0,
        myftp::kHeaderLength + payload_length);

    if (!myftp::send_all(socket_fd, &request, sizeof(request)) ||
        !myftp::send_all(socket_fd, filename.c_str(), payload_length)) {
        std::cerr << "failed to send GET_REQUEST" << std::endl;
        return false;
    }

    // 先接收 GET_REPLY 消息头，判断服务器是否存在该文件。如果不存在，则返回 true，表示请求成功，但文件不存在。如果存在，则继续接收 FILE_DATA 消息头和有效负载，即文件数据，并将其写入本地文件中。
    myftp::Header reply{};
    if (!myftp::recv_all(socket_fd, &reply, sizeof(reply))) {
        std::cerr << "failed to receive GET_REPLY" << std::endl;
        return false;
    }

    if (!myftp::has_valid_magic(reply) ||
        reply.m_type != static_cast<std::uint8_t>(
                            myftp::MessageType::GET_REPLY) ||
        (reply.m_status != 0 && reply.m_status != 1) ||
        myftp::total_length(reply) != myftp::kHeaderLength) {
        std::cerr << "invalid GET_REPLY" << std::endl;
        return false;
    }

    if (reply.m_status == 0) {
        std::cerr << "server file does not exist" << std::endl;
        return true;
    }

    // 如果存在文件，下一步先接收 FILE_DATA 消息头，判断其总长度是否大于等于协议头长度（12 字节），否则视为无效消息，返回 false。然后计算文件数据的有效负载长度，并接收文件数据的有效负载部分。如果接收失败，则返回 false。最后将接收到的文件数据写入本地文件中，如果写入失败，则返回 false。
    myftp::Header data_header{};
    if (!myftp::recv_all(socket_fd, &data_header, sizeof(data_header))) {
        std::cerr << "failed to receive FILE_DATA header" << std::endl;
        return false;
    }

    if (!myftp::has_valid_magic(data_header) ||
        data_header.m_type != static_cast<std::uint8_t>(
                                  myftp::MessageType::FILE_DATA)) {
        std::cerr << "invalid FILE_DATA message" << std::endl;
        return false;
    }

    const std::uint32_t data_message_length =
        myftp::total_length(data_header);
    if (data_message_length < myftp::kHeaderLength) {
        std::cerr << "invalid FILE_DATA length" << std::endl;
        return false;
    }

    const std::uint32_t file_size =
        data_message_length - myftp::kHeaderLength;
    std::vector<std::uint8_t> file_data(file_size);

    // 最后再接收 FILE_DATA 消息的有效负载部分，即文件数据。如果接收失败，则返回 false。然后将接收到的文件数据写入本地文件中，如果写入失败，则返回 false。最后返回 true，表示文件下载成功。
    if (file_size > 0 &&
        !myftp::recv_all(socket_fd, file_data.data(), file_data.size())) {
        std::cerr << "failed to receive file data" << std::endl;
        return false;
    }

    // Files may be ASCII or binary, and an existing local file must be
    // overwritten.
    std::ofstream output(filename,
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "failed to open local file for writing: "
                  << filename << std::endl;
        return false;
    }

    if (!file_data.empty()) {
        output.write(reinterpret_cast<const char*>(file_data.data()),
                     static_cast<std::streamsize>(file_data.size()));
    }

    if (!output) {
        std::cerr << "failed to write local file: " << filename << std::endl;
        return false;
    }

    return true;
}

bool perform_put_request(int socket_fd, const std::string& filename) {
    // The Client must check that the local file exists before sending
    // PUT_REQUEST.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(filename, ec) || ec) {
        std::cerr << "local file does not exist: " << filename << std::endl;
        return true;
    }

    const std::uintmax_t file_size = std::filesystem::file_size(filename, ec);
    if (ec ||
        file_size >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::uint32_t>::max() -
                myftp::kHeaderLength)) {
        std::cerr << "failed to determine local file size: "
                  << filename << std::endl;
        return false;
    }

    std::ifstream input_file(filename, std::ios::binary);
    if (!input_file) {
        std::cerr << "failed to open local file for reading: "
                  << filename << std::endl;
        return true;
    }

    // PUT_REQUEST carries one null-terminated file name as payload.
    const std::uint32_t payload_length =
        static_cast<std::uint32_t>(filename.size() + 1);
    const myftp::Header request = myftp::make_header(
        myftp::MessageType::PUT_REQUEST,
        0,
        myftp::kHeaderLength + payload_length);

    if (!myftp::send_all(socket_fd, &request, sizeof(request)) ||
        !myftp::send_all(socket_fd, filename.c_str(), payload_length)) {
        std::cerr << "failed to send PUT_REQUEST" << std::endl;
        return false;
    }

    // The Server acknowledges PUT_REQUEST before the Client sends FILE_DATA.
    myftp::Header reply{};
    if (!myftp::recv_all(socket_fd, &reply, sizeof(reply))) {
        std::cerr << "failed to receive PUT_REPLY" << std::endl;
        return false;
    }

    if (!myftp::has_valid_magic(reply) ||
        reply.m_type != static_cast<std::uint8_t>(
                            myftp::MessageType::PUT_REPLY) ||
        myftp::total_length(reply) != myftp::kHeaderLength) {
        std::cerr << "invalid PUT_REPLY" << std::endl;
        return false;
    }

    // 如果 PUT_REPLY 消息头有效，则发送 FILE_DATA 消息头，告诉服务器接下来要发送的文件数据的总长度（包括协议头和有效负载）。然后将文件数据分块发送给服务器，直到文件数据发送完毕。如果发送失败，则返回 false。最后返回 true，表示文件上传成功。
    const myftp::Header data_header = myftp::make_header(
        myftp::MessageType::FILE_DATA,
        0,
        myftp::kHeaderLength + static_cast<std::uint32_t>(file_size));

    if (!myftp::send_all(socket_fd, &data_header, sizeof(data_header))) {
        std::cerr << "failed to send FILE_DATA header" << std::endl;
        return false;
    }

    // Send the file payload in chunks. The file is opened in binary mode, so
    // arbitrary byte values (including '\0') are transferred unchanged.
    char buffer[8192];
    while (input_file) {
        input_file.read(buffer, sizeof(buffer));
        const std::streamsize count = input_file.gcount();
        if (count > 0 &&
            !myftp::send_all(socket_fd,
                             buffer,
                             static_cast<std::size_t>(count))) {
            std::cerr << "failed to send file data" << std::endl;
            return false;
        }
    }

    if (!input_file.eof()) {
        std::cerr << "failed to read local file: " << filename << std::endl;
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

        if (command == "ls") {
            if (socket_fd < 0) {
                std::cerr << "no myFTP connection is open" << std::endl;
                continue;
            }

            std::string extra;
            if (input >> extra) {
                std::cerr << "usage: ls" << std::endl;
                continue;
            }

            perform_list_request(socket_fd);
            continue;
        }

        if (command == "cd") {
            if (socket_fd < 0) {
                std::cerr << "no myFTP connection is open" << std::endl;
                continue;
            }

            std::string directory;
            std::string extra;
            if (!(input >> directory) || (input >> extra)) {
                std::cerr << "usage: cd <directory>" << std::endl;
                continue;
            }

            perform_change_dir_request(socket_fd, directory);
            continue;
        }

        if (command == "get") {
            if (socket_fd < 0) {
                std::cerr << "no myFTP connection is open" << std::endl;
                continue;
            }

            std::string filename;
            std::string extra;
            if (!(input >> filename) || (input >> extra)) {
                std::cerr << "usage: get <filename>" << std::endl;
                continue;
            }

            perform_get_request(socket_fd, filename);
            continue;
        }

        if (command == "put") {
            if (socket_fd < 0) {
                std::cerr << "no myFTP connection is open" << std::endl;
                continue;
            }

            std::string filename;
            std::string extra;
            if (!(input >> filename) || (input >> extra)) {
                std::cerr << "usage: put <filename>" << std::endl;
                continue;
            }

            perform_put_request(socket_fd, filename);
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
