#include <Alryn/Net/ByteBuffer.h>

#include <bit>

namespace alryn::net {

void ByteWriter::write_u8(u8 v) {
    data_.push_back(v);
}

void ByteWriter::write_u16(u16 v) {
    data_.push_back(static_cast<u8>(v & 0xFF));
    data_.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

void ByteWriter::write_u32(u32 v) {
    for (int i = 0; i < 4; ++i) {
        data_.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
    }
}

void ByteWriter::write_f32(f32 v) {
    write_u32(std::bit_cast<u32>(v));
}

void ByteWriter::write_vec3(const Vec3& v) {
    write_f32(v.x);
    write_f32(v.y);
    write_f32(v.z);
}

bool ByteReader::require(usize n) {
    if (pos_ + n > size_) {
        ok_ = false;
        return false;
    }
    return true;
}

u8 ByteReader::read_u8() {
    if (!require(1)) {
        return 0;
    }
    return data_[pos_++];
}

u16 ByteReader::read_u16() {
    if (!require(2)) {
        return 0;
    }
    const u16 v = static_cast<u16>(data_[pos_]) | (static_cast<u16>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

u32 ByteReader::read_u32() {
    if (!require(4)) {
        return 0;
    }
    u32 v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<u32>(data_[pos_ + static_cast<usize>(i)]) << (8 * i);
    }
    pos_ += 4;
    return v;
}

f32 ByteReader::read_f32() {
    return std::bit_cast<f32>(read_u32());
}

Vec3 ByteReader::read_vec3() {
    Vec3 v;
    v.x = read_f32();
    v.y = read_f32();
    v.z = read_f32();
    return v;
}

} // namespace alryn::net
