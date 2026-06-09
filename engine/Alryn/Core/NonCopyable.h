#pragma once

namespace alryn {

// Base for types that own unique resources: copying is forbidden, moving is
// allowed (so they can still live in containers / be returned by value).
class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

    NonCopyable(NonCopyable&&) = default;
    NonCopyable& operator=(NonCopyable&&) = default;
};

// Base for types pinned in memory: neither copyable nor movable.
class NonMovable {
protected:
    NonMovable() = default;
    ~NonMovable() = default;

public:
    NonMovable(const NonMovable&) = delete;
    NonMovable& operator=(const NonMovable&) = delete;
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

} // namespace alryn
