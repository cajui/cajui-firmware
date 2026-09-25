#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace cajui {
// Bounded printf-style builder over a caller buffer. Any truncation or encoding error
// invalidates the whole document; callers check ok() and never use a partial result.
class TextBuffer {
public:
    TextBuffer(char* output, size_t capacity) : output_(output), capacity_(capacity) {
        if (capacity_) output_[0] = 0;
        ok_ = capacity_ != 0;
    }
    // NOLINTNEXTLINE(cert-dcl50-cpp): bounded printf-style helper over vsnprintf.
    void format(const char* pattern, ...) __attribute__((format(printf, 2, 3))) {
        if (!ok_) return;
        va_list arguments;
        va_start(arguments, pattern);
        const int written = std::vsnprintf(output_ + used_, capacity_ - used_, pattern, arguments);
        va_end(arguments);
        if (written < 0 || size_t(written) >= capacity_ - used_)
            ok_ = false;
        else
            used_ += size_t(written);
    }
    bool ok() const { return ok_; }
    size_t size() const { return used_; }

private:
    char* output_;
    size_t capacity_, used_ = 0;
    bool ok_ = true;
};
} // namespace cajui
