#pragma once
#include <cstring>
#include <vector>

#include "../common/config.h"
#include "unistd.h"

// Efficient non-contiguous buffer for network I/O with automatic compaction
class Buffer {
public:
    Buffer() : buffer_(INITIAL_BUFFER_SIZE), reader_index_(0), writer_index_(0) {}

    // Basic accessors for the ring buffer positions
    size_t readable_bytes() const { return writer_index_ - reader_index_; }
    size_t writable_bytes() const { return buffer_.size() - writer_index_; }
    size_t prependable_bytes() const { return reader_index_; }

    char *begin_write() { return begin() + writer_index_; }
    const char *begin_write() const { return begin() + writer_index_; }

    const char *peek() const { return begin() + reader_index_; }

    // Protocol parsing helpers: find CRLF or newline
    const char *find_crtf() const {
        const char *crlf = std::search(peek(), begin_write(), CRLF, CRLF + 2);
        return crlf == begin_write() ? nullptr : crlf;
    }

    const char *find_eol() const {
        const void *eol = memchr(peek(), '\n', readable_bytes());
        return static_cast<const char *>(eol);
    }

    // Consume data from the buffer (advance reader index)
    void retrieve(const size_t len) {
        if (len < readable_bytes()) {
            reader_index_ += len;
        } else {
            retrieve_all();
        }
    }

    void retrieve_until(const char *end) { retrieve(end - peek()); }

    void retrieve_all() {
        reader_index_ = 0;
        writer_index_ = 0;
    }

    // Extract data and consume it atomically
    std::string retrieve_as_string(const size_t len) {
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    std::string retrieve_all_as_string() { return retrieve_as_string(readable_bytes()); }

    // Append external data, triggering resize if needed
    void append(const char *data, const size_t len) {
        ensure_writable_bytes(len);
        std::copy_n(data, len, begin_write());
        writer_index_ += len;
    }

    void append(const std::string &data) { append(data.data(), data.size()); }

    void ensure_writable_bytes(const size_t len) {
        if (writable_bytes() < len) {
            make_space(len);
        }
    }

    void has_written(size_t len) { writer_index_ += len; }

    // Memory optimization: shrink large unused buffers
    void shrink_if_needed() {
        if (buffer_.size() > HIGH_WATER_MARK && readable_bytes() < LOW_WATER_MARK) {
            buffer_.shrink_to_fit();
        }
    }

    ssize_t read_fd(int fd, int *saved_errno) {
        *saved_errno = 0;
        ssize_t total = 0;

        while (true) {
            ensure_writable_bytes(READ_CHUNK_SIZE);
            const ssize_t n = ::read(fd, begin_write(), writable_bytes());

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                *saved_errno = errno;
                return total > 0 ? total : -1;
            }
            if (n == 0)
                return total;

            has_written(n);
            total += n;

            if (static_cast<size_t>(n) < READ_CHUNK_SIZE / 2)
                break;
        }

        return total;
    }

    std::string_view peek_view(size_t len) const {
        return {peek(),std::min(len,readable_bytes())};
    }

    std::string_view peek_view() const {
        return {peek(),readable_bytes()};
    }

private:
    static constexpr char CRLF[] = "\r\n";

    std::vector<char> buffer_; // Underlying storage
    size_t reader_index_;
    size_t writer_index_;

    char *begin() { return buffer_.data(); }
    const char *begin() const { return buffer_.data(); }

    // Either resize or move data to front to make room
    void make_space(const size_t len) {
        if (writable_bytes() >= len)
            return;

        const size_t readable = readable_bytes();
        constexpr size_t prepend_threshold = 256 * 1024;

        if (prependable_bytes() >= prepend_threshold) {
            if (readable > 0) {
                std::memmove(begin(), peek(), readable);
            }
            reader_index_ = 0;
            writer_index_ = readable;
        }

        if (writable_bytes() + prependable_bytes() < len) {
            size_t new_size = buffer_.size() * 3 / 2 + len;
            new_size = std::max(new_size, INITIAL_BUFFER_SIZE * 2);
            new_size = std::min(new_size, MAX_BUFFER_SIZE);
            buffer_.resize(new_size);
        }
    }
};
