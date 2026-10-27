#include "gzip_stream.h"

#include <cstring>
#include <new>
#include "esp_rom_crc.h"

namespace {

// RFC 1951, 3.2.5: base value and extra bits of the length codes 257..285
// and of the distance codes 0..29.
constexpr uint16_t LENGTH_BASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t LENGTH_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t DIST_BASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}  // namespace

bool GzipStream::begin(Sink sink)
{
    sink_ = std::move(sink);
    window_.reset(new (std::nothrow) uint8_t[BUFFER]);
    head_.reset(new (std::nothrow) uint16_t[1u << HASH_BITS]);
    if (!window_ || !head_) {
        window_.reset();
        head_.reset();
        return false;
    }
    memset(head_.get(), 0, sizeof(uint16_t) << HASH_BITS);
    pos_ = end_ = 0;
    crc_ = 0;
    size_ = 0;
    bit_buf_ = 0;
    bit_count_ = 0;
    out_len_ = 0;
    ok_ = true;

    // RFC 1952 header: deflate, no flags, no time, Unix.
    static const uint8_t HEADER[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    for (const uint8_t b : HEADER) {
        put_byte(b);
    }
    // One fixed-Huffman block for all the data (BFINAL = 0, BTYPE = 01);
    // finish() closes it and adds an empty final block.
    put_bits(0, 1);
    put_bits(1, 2);
    return ok_;
}

bool GzipStream::write(const void *data, size_t len)
{
    const auto *in = static_cast<const uint8_t *>(data);
    crc_ = esp_rom_crc32_le(crc_, in, len);
    size_ += static_cast<uint32_t>(len);
    while (ok_ && len > 0) {
        if (end_ == BUFFER) {
            slide();
        }
        const size_t n = len < BUFFER - end_ ? len : BUFFER - end_;
        memcpy(window_.get() + end_, in, n);
        end_ += n;
        in += n;
        len -= n;
        if (end_ - pos_ >= MAX_MATCH) {
            compress(false);
        }
    }
    return ok_;
}

bool GzipStream::finish()
{
    if (!ok_) {
        return false;
    }
    compress(true);
    put_code(0, 7);  // end of block (256)
    put_bits(1, 1);  // final, empty fixed-Huffman block
    put_bits(1, 2);
    put_code(0, 7);
    if (bit_count_ > 0) {
        put_bits(0, 8 - bit_count_);  // byte boundary
    }
    for (int i = 0; i < 4; i++) {
        put_byte(static_cast<uint8_t>(crc_ >> (8 * i)));
    }
    for (int i = 0; i < 4; i++) {
        put_byte(static_cast<uint8_t>(size_ >> (8 * i)));
    }
    flush_out();
    window_.reset();
    head_.reset();
    return ok_;
}

// Encodes the data up to MAX_MATCH bytes before the end (all of it when
// `final`), so a match can always look far enough ahead.
void GzipStream::compress(bool final)
{
    const uint8_t *w = window_.get();
    const size_t limit = final ? end_ : end_ - MAX_MATCH;
    while (ok_ && pos_ < limit) {
        const size_t avail = end_ - pos_;
        size_t best = 0, dist = 0;
        if (avail >= MIN_MATCH) {
            const uint32_t h = ((w[pos_] << 8) ^ (w[pos_ + 1] << 4) ^ w[pos_ + 2]) & ((1u << HASH_BITS) - 1);
            const size_t cand = head_[h];
            head_[h] = static_cast<uint16_t>(pos_ + 1);
            if (cand != 0 && pos_ - (cand - 1) <= WINDOW) {
                const size_t c = cand - 1;
                const size_t max = avail < MAX_MATCH ? avail : MAX_MATCH;
                while (best < max && w[c + best] == w[pos_ + best]) {
                    best++;
                }
                dist = pos_ - c;
            }
        }
        if (best >= MIN_MATCH) {
            put_match(best, dist);
            // Index the positions inside the match too, so the next lines
            // find their match in this one.
            for (size_t p = pos_ + 1; p < pos_ + best && p + 2 < end_; p++) {
                const uint32_t h = ((w[p] << 8) ^ (w[p + 1] << 4) ^ w[p + 2]) & ((1u << HASH_BITS) - 1);
                head_[h] = static_cast<uint16_t>(p + 1);
            }
            pos_ += best;
        } else {
            put_literal(w[pos_]);
            pos_++;
        }
    }
}

// Drops the oldest WINDOW bytes; compress() has left pos_ beyond them.
void GzipStream::slide()
{
    memmove(window_.get(), window_.get() + WINDOW, end_ - WINDOW);
    pos_ -= WINDOW;
    end_ -= WINDOW;
    for (size_t i = 0; i < (1u << HASH_BITS); i++) {
        head_[i] = head_[i] > WINDOW ? static_cast<uint16_t>(head_[i] - WINDOW) : 0;
    }
}

void GzipStream::put_bits(uint32_t value, unsigned count)
{
    bit_buf_ |= value << bit_count_;
    bit_count_ += count;
    while (bit_count_ >= 8) {
        put_byte(static_cast<uint8_t>(bit_buf_));
        bit_buf_ >>= 8;
        bit_count_ -= 8;
    }
}

void GzipStream::put_code(uint32_t code, unsigned count)
{
    uint32_t reversed = 0;
    for (unsigned i = 0; i < count; i++) {
        reversed = (reversed << 1) | ((code >> i) & 1);
    }
    put_bits(reversed, count);
}

// RFC 1951, 3.2.6: fixed literal/length codes.
void GzipStream::put_literal(unsigned value)
{
    if (value < 144) {
        put_code(0x30 + value, 8);
    } else if (value < 256) {
        put_code(0x190 + value - 144, 9);
    } else if (value < 280) {
        put_code(value - 256, 7);
    } else {
        put_code(0xc0 + value - 280, 8);
    }
}

void GzipStream::put_match(size_t length, size_t distance)
{
    unsigned l = 28;
    while (LENGTH_BASE[l] > length) {
        l--;
    }
    put_literal(257 + l);
    put_bits(static_cast<uint32_t>(length - LENGTH_BASE[l]), LENGTH_EXTRA[l]);

    unsigned d = 29;
    while (DIST_BASE[d] > distance) {
        d--;
    }
    put_code(d, 5);
    put_bits(static_cast<uint32_t>(distance - DIST_BASE[d]), DIST_EXTRA[d]);
}

void GzipStream::put_byte(uint8_t b)
{
    out_[out_len_++] = b;
    if (out_len_ == OUT_SIZE) {
        flush_out();
    }
}

void GzipStream::flush_out()
{
    if (ok_ && out_len_ > 0) {
        ok_ = sink_(out_, out_len_);
    }
    out_len_ = 0;
}
