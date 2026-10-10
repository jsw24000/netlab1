#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>

namespace myftp {
// myFTP 协议定义

constexpr std::size_t kMagicNumberLength = 6; // sizeof(Header::m_protocol) must be exactly 6 bytes
constexpr std::uint32_t kHeaderLength = 12; // sizeof(Header) must be exactly 12 bytes

constexpr std::uint8_t kMagicNumber[kMagicNumberLength] = {
    0xC1, 0xA1, 0x10, 'f', 't', 'p'
};

enum class MessageType : std::uint8_t {
    OPEN_CONN_REQUEST = 0xA1,
    OPEN_CONN_REPLY = 0xA2,
    LIST_REQUEST = 0xA3,
    LIST_REPLY = 0xA4,
    CHANGE_DIR_REQUEST = 0xA5,
    CHANGE_DIR_REPLY = 0xA6,
    GET_REQUEST = 0xA7,
    GET_REPLY = 0xA8,
    PUT_REQUEST = 0xA9,
    PUT_REPLY = 0xAA,
    SHA_REQUEST = 0xAB,
    SHA_REPLY = 0xAC,
    QUIT_REQUEST = 0xAD,
    QUIT_REPLY = 0xAE,
    FILE_DATA = 0xFF,
};

struct __attribute__((packed)) Header {
    std::uint8_t m_protocol[kMagicNumberLength];
    std::uint8_t m_type;
    std::uint8_t m_status;
    std::uint32_t m_length;
};

static_assert(sizeof(Header) == kHeaderLength,
              "myFTP header must be exactly 12 bytes");

inline Header make_header(MessageType type,
                          std::uint8_t status,
                          std::uint32_t total_length) {
    Header header{};
    std::memcpy(header.m_protocol, kMagicNumber, kMagicNumberLength); // 将 myFTP 协议的魔数（magic number）复制到消息头的 m_protocol 字段中
    // 其余的字段包括消息类型（m_type）、状态码（m_status）和消息总长度（m_length）。消息总长度是指整个 myFTP 消息的字节数，包括协议头和有效负载。
    header.m_type = static_cast<std::uint8_t>(type);
    header.m_status = status;
    header.m_length = htonl(total_length);
    return header;
}

inline bool has_valid_magic(const Header& header) {
    return std::memcmp(header.m_protocol,
                       kMagicNumber,
                       kMagicNumberLength) == 0;
}

inline std::uint32_t total_length(const Header& header) {
    return ntohl(header.m_length);
    // 将 server 返回的 head 中的 m_length 字段从网络字节序转换为主机字节序，并返回转换后的值。这个函数用于获取 myFTP 消息的总长度，以便在接收消息时知道需要读取多少字节的数据。
}

// TCP is a byte stream: one send() is not guaranteed to be matched by one
// recv(). These helpers keep calling send()/recv() until exactly length bytes
// have been transferred, or an error/connection close occurs.
inline bool send_all(int socket_fd, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent_total = 0;

    while (sent_total < length) {
        const ssize_t sent = send(socket_fd,
                                  bytes + sent_total,
                                  length - sent_total,
                                  MSG_NOSIGNAL);
        if (sent > 0) {
            sent_total += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }

    return true;
}

inline bool recv_all(int socket_fd, void* data, std::size_t length) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t received_total = 0;

    while (received_total < length) {
        const ssize_t received = recv(socket_fd,
                                      bytes + received_total,
                                      length - received_total,
                                      0);
        if (received > 0) {
            received_total += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        // received == 0 means the peer closed the TCP connection.
        return false;
    }

    return true;
}

}  // namespace myftp
