#pragma once

// Abstract transport interface for Guest↔Host command stream communication.
// Implementations: TCP (Phase 1), HGCM (Phase 1 alt), PCI shared memory (Phase 3).

#include <cstdint>
#include <cstddef>

class ITransportSender {
public:
    virtual ~ITransportSender() = default;

    // Send a complete command stream buffer. Returns false on error.
    virtual bool send(const uint8_t* data, size_t size) = 0;

    // Receive a response from host (e.g. image index after acquire).
    // Returns bytes read, 0 on error.
    virtual size_t recv(uint8_t* buf, size_t maxSize) = 0;
};

class ITransportReceiver {
public:
    virtual ~ITransportReceiver() = default;

    // Wait for and receive a command stream buffer.
    // Caller owns the buffer. Returns false on disconnect.
    virtual bool recv(uint8_t* buf, size_t bufSize, size_t& bytesRead) = 0;

    // Send a response back to guest.
    virtual bool send(const uint8_t* data, size_t size) = 0;
};
