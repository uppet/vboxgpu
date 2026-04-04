#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>

// Write-side: accumulate command bytes
class VnStreamWriter {
public:
    void writeU32(uint32_t v) {
        auto off = buf_.size();
        buf_.resize(off + 4);
        memcpy(buf_.data() + off, &v, 4);
    }

    void writeU64(uint64_t v) {
        auto off = buf_.size();
        buf_.resize(off + 8);
        memcpy(buf_.data() + off, &v, 8);
    }

    void writeI32(int32_t v) { writeU32(static_cast<uint32_t>(v)); }

    void writeF32(float v) {
        uint32_t bits;
        memcpy(&bits, &v, 4);
        writeU32(bits);
    }

    void writeBytes(const void* data, size_t size) {
        // 4-byte aligned write
        size_t aligned = (size + 3) & ~size_t(3);
        auto off = buf_.size();
        buf_.resize(off + aligned, 0);
        memcpy(buf_.data() + off, data, size);
    }

    // Begin a command: write header, return offset for patching size
    size_t beginCommand(uint32_t cmdType) {
        writeU32(cmdType);
        size_t sizeOffset = buf_.size();
        writeU32(0); // placeholder for size
        return sizeOffset;
    }

    // End a command: patch the size field
    void endCommand(size_t sizeOffset) {
        uint32_t totalSize = static_cast<uint32_t>(buf_.size() - sizeOffset + 4);
        memcpy(buf_.data() + sizeOffset, &totalSize, 4);
    }

    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }
    std::vector<uint8_t>& buffer() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

// Read-side: consume command bytes
class VnStreamReader {
public:
    VnStreamReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    bool hasMore() const { return pos_ < size_; }
    size_t remaining() const { return size_ - pos_; }

    uint32_t readU32() {
        check(4);
        uint32_t v;
        memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return v;
    }

    uint64_t readU64() {
        check(8);
        uint64_t v;
        memcpy(&v, data_ + pos_, 8);
        pos_ += 8;
        return v;
    }

    int32_t readI32() { return static_cast<int32_t>(readU32()); }

    float readF32() {
        uint32_t bits = readU32();
        float v;
        memcpy(&v, &bits, 4);
        return v;
    }

    void readBytes(void* dst, size_t size) {
        size_t aligned = (size + 3) & ~size_t(3);
        check(aligned);
        memcpy(dst, data_ + pos_, size);
        pos_ += aligned;
    }

    void skip(size_t bytes) {
        size_t aligned = (bytes + 3) & ~size_t(3);
        check(aligned);
        pos_ += aligned;
    }

    // Skip exact bytes without alignment (for command framing)
    void skipExact(size_t bytes) {
        check(bytes);
        pos_ += bytes;
    }

    void setPos(size_t pos) { pos_ = pos; }

    const uint8_t* currentPtr() const { return data_ + pos_; }

private:
    void check(size_t need) {
        if (pos_ + need > size_)
            throw std::runtime_error("VnStreamReader: read past end");
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};
