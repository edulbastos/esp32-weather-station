// Streaming gzip compressor for the CSV download (FR-8.8).
// A compact deflate encoder (RFC 1951/1952): LZ77 with a 4 KB window and a
// single hash head per 3-byte prefix, fixed Huffman codes. About 16 KB of
// RAM while in use; the miniz compressor in the ESP32 ROM needs about 300 KB.
// Good enough for repetitive text such as the history CSV.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class GzipStream {
public:
    // Receives the compressed bytes; returns false to abort.
    using Sink = std::function<bool(const uint8_t *data, size_t len)>;

    // Allocates the buffers and writes the gzip header. Returns false if
    // there is not enough memory or the sink fails.
    bool begin(Sink sink);

    // Compresses `len` bytes. Returns false once the sink has failed.
    bool write(const void *data, size_t len);

    // Compresses what is left and writes the end of the stream (CRC-32 and
    // length). Returns false if the sink failed.
    bool finish();

private:
    static constexpr size_t WINDOW = 4096;          // largest match distance
    static constexpr size_t BUFFER = 2 * WINDOW;    // window + new data
    static constexpr size_t MIN_MATCH = 3;
    static constexpr size_t MAX_MATCH = 258;
    static constexpr unsigned HASH_BITS = 12;
    static constexpr size_t OUT_SIZE = 256;

    void compress(bool final);
    void slide();
    void put_bits(uint32_t value, unsigned count);
    void put_code(uint32_t code, unsigned count);  // Huffman code, MSB first
    void put_literal(unsigned value);
    void put_match(size_t length, size_t distance);
    void put_byte(uint8_t b);
    void flush_out();

    Sink sink_;
    std::unique_ptr<uint8_t[]> window_;   // BUFFER bytes
    std::unique_ptr<uint16_t[]> head_;    // position + 1 per hash, 0 = none
    size_t pos_ = 0;                      // next byte to encode
    size_t end_ = 0;                      // end of the data in window_
    uint32_t crc_ = 0;
    uint32_t size_ = 0;                   // uncompressed length mod 2^32
    uint32_t bit_buf_ = 0;
    unsigned bit_count_ = 0;
    uint8_t out_[OUT_SIZE];
    size_t out_len_ = 0;
    bool ok_ = false;
};
