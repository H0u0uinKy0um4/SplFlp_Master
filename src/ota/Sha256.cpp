#include "ota/Sha256.h"

#include <cstring>

namespace sf {

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL,
};

uint32_t rotr(uint32_t value, uint8_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

uint32_t loadBe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void storeBe32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

constexpr char HEX[] = "0123456789abcdef";

}  // namespace

void Sha256::reset() {
    state_[0] = 0x6a09e667UL;
    state_[1] = 0xbb67ae85UL;
    state_[2] = 0x3c6ef372UL;
    state_[3] = 0xa54ff53aUL;
    state_[4] = 0x510e527fUL;
    state_[5] = 0x9b05688cUL;
    state_[6] = 0x1f83d9abUL;
    state_[7] = 0x5be0cd19UL;
    memset(buffer_, 0, sizeof(buffer_));
    buffered_ = 0;
    bitLength_ = 0;
}

void Sha256::block(const uint8_t bytes[64]) {
    uint32_t w[64] = {};
    for (uint8_t i = 0; i < 16; i++) w[i] = loadBe32(bytes + 4 * i);
    for (uint8_t i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                            (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                            (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (uint8_t i = 0; i < 64; i++) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + K[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    bitLength_ += (uint64_t)len * 8;
    while (len > 0) {
        const size_t room = sizeof(buffer_) - buffered_;
        const size_t take = len < room ? len : room;
        memcpy(buffer_ + buffered_, data, take);
        buffered_ += take;
        data += take;
        len -= take;
        if (buffered_ == sizeof(buffer_)) {
            block(buffer_);
            buffered_ = 0;
        }
    }
}

void Sha256::digest(uint8_t out[32]) const {
    Sha256 padded = *this;
    padded.buffer_[padded.buffered_++] = 0x80;
    if (padded.buffered_ > 56) {
        memset(padded.buffer_ + padded.buffered_, 0,
               sizeof(padded.buffer_) - padded.buffered_);
        padded.block(padded.buffer_);
        padded.buffered_ = 0;
    }
    memset(padded.buffer_ + padded.buffered_, 0, 56 - padded.buffered_);
    const uint64_t bits = padded.bitLength_;
    storeBe32(padded.buffer_ + 56, (uint32_t)(bits >> 32));
    storeBe32(padded.buffer_ + 60, (uint32_t)bits);
    padded.block(padded.buffer_);

    for (uint8_t i = 0; i < 8; i++) storeBe32(out + 4 * i, padded.state_[i]);
}

void Sha256::hex(char out[65]) const {
    uint8_t bytes[32];
    digest(bytes);
    for (uint8_t i = 0; i < 32; i++) {
        out[2 * i] = HEX[bytes[i] >> 4];
        out[2 * i + 1] = HEX[bytes[i] & 0x0F];
    }
    out[64] = '\0';
}

}  // namespace sf
