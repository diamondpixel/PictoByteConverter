#pragma once

namespace debug {
struct NullMutex {
    void lock() noexcept {}
    void unlock() noexcept {}
    bool try_lock() noexcept { return true; }
};

template<typename M = NullMutex>
class NullLockGuard {
public:
    explicit NullLockGuard(M&) noexcept {}
    ~NullLockGuard() noexcept = default;
    NullLockGuard(const NullLockGuard&) = delete;
    NullLockGuard& operator=(const NullLockGuard&) = delete;
};
}
