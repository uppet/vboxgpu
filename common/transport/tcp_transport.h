#pragma once

// TCP transport for Guest↔Host communication.
// Protocol: each message is framed as [uint32_t length][payload...].

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "transport.h"
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

constexpr uint16_t DEFAULT_PORT = 19800;

// Frame protocol: [4 bytes: payload size (little-endian uint32)] [payload]
// Response from host after each frame: [4 bytes: uint32 image_index]

static inline bool tcp_send_all(SOCKET s, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        int n = ::send(s, reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(size - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static inline bool tcp_recv_all(SOCKET s, uint8_t* buf, size_t size) {
    size_t got = 0;
    while (got < size) {
        int n = ::recv(s, reinterpret_cast<char*>(buf + got),
                       static_cast<int>(size - got), 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

static inline bool tcp_send_framed(SOCKET s, const uint8_t* data, size_t size) {
    uint32_t len = static_cast<uint32_t>(size);
    if (!tcp_send_all(s, reinterpret_cast<const uint8_t*>(&len), 4)) return false;
    if (size > 0 && !tcp_send_all(s, data, size)) return false;
    return true;
}

static inline bool tcp_recv_framed(SOCKET s, uint8_t* buf, size_t bufSize, size_t& bytesRead) {
    uint32_t len = 0;
    if (!tcp_recv_all(s, reinterpret_cast<uint8_t*>(&len), 4)) return false;
    if (len > bufSize) return false;
    if (len > 0 && !tcp_recv_all(s, buf, len)) return false;
    bytesRead = len;
    return true;
}

// --- TCP Client (Guest side) ---

class TcpSender : public ITransportSender {
public:
    bool connect(const char* host, uint16_t port) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);

        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            fprintf(stderr, "[TcpSender] Connect failed: %d\n", WSAGetLastError());
            return false;
        }
        fprintf(stderr, "[TcpSender] Connected to %s:%u\n", host, port);
        return true;
    }

    bool send(const uint8_t* data, size_t size) override {
        return tcp_send_framed(sock_, data, size);
    }

    size_t recv(uint8_t* buf, size_t maxSize) override {
        size_t got = 0;
        if (!tcp_recv_framed(sock_, buf, maxSize, got)) return 0;
        return got;
    }

    void close() {
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        WSACleanup();
    }

    ~TcpSender() { close(); }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

// --- TCP Server (Host side) ---

class TcpReceiver : public ITransportReceiver {
public:
    bool listen(uint16_t port) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        listenSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock_ == INVALID_SOCKET) return false;

        int yes = 1;
        setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            fprintf(stderr, "[TcpReceiver] Bind failed: %d\n", WSAGetLastError());
            return false;
        }
        if (::listen(listenSock_, 1) != 0) return false;

        fprintf(stderr, "[TcpReceiver] Listening on port %u...\n", port);
        return true;
    }

    bool accept() {
        clientSock_ = ::accept(listenSock_, nullptr, nullptr);
        if (clientSock_ == INVALID_SOCKET) return false;
        fprintf(stderr, "[TcpReceiver] Client connected.\n");
        return true;
    }

    bool recv(uint8_t* buf, size_t bufSize, size_t& bytesRead) override {
        return tcp_recv_framed(clientSock_, buf, bufSize, bytesRead);
    }

    bool send(const uint8_t* data, size_t size) override {
        return tcp_send_framed(clientSock_, data, size);
    }

    void close() {
        if (clientSock_ != INVALID_SOCKET) { closesocket(clientSock_); clientSock_ = INVALID_SOCKET; }
        if (listenSock_ != INVALID_SOCKET) { closesocket(listenSock_); listenSock_ = INVALID_SOCKET; }
        WSACleanup();
    }

    ~TcpReceiver() { close(); }

private:
    SOCKET listenSock_ = INVALID_SOCKET;
    SOCKET clientSock_ = INVALID_SOCKET;
};
