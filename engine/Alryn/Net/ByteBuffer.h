#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <vector>

namespace alryn::net {

// Little-endian binary writer for network messages.
class ByteWriter {
public:
    void write_u8(u8 v);
    void write_u16(u16 v);
    void write_u32(u32 v);
    void write_f32(f32 v);
    void write_bool(bool v) { write_u8(v ? 1 : 0); }
    void write_vec3(const Vec3& v);

    const std::vector<u8>& data() const { return data_; }
    const u8* bytes() const { return data_.data(); }
    usize size() const { return data_.size(); }

private:
    std::vector<u8> data_;
};

// Bounds-checked little-endian reader. Any over-read flips ok() to false and
// subsequent reads return zero, so malformed packets fail safely.
class ByteReader {
public:
    ByteReader(const u8* data, usize size) : data_(data), size_(size) {}

    u8 read_u8();
    u16 read_u16();
    u32 read_u32();
    f32 read_f32();
    bool read_bool() { return read_u8() != 0; }
    Vec3 read_vec3();

    bool ok() const { return ok_; }
    usize remaining() const { return pos_ <= size_ ? size_ - pos_ : 0; }

private:
    bool require(usize n);

    const u8* data_;
    usize size_;
    usize pos_ = 0;
    bool ok_ = true;
};

} // namespace alryn::net
