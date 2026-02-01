#pragma once
#include <cstring>
#include <vector>

#include "config.h"

class Buffer {
public:
    Buffer():buffer_(INITIAL_BUFFER_SIZE),reader_index_(0),writer_index_(0){}

    size_t readable_bytes() const {return writer_index_ - reader_index_;}
    size_t writable_bytes() const {return buffer_.size() - writer_index_;}
    size_t prependable_bytes() const {return reader_index_;}

    char* begin_write(){return begin() + writer_index_;}
    const char* begin_write() const {return begin()+writer_index_;}

    const char* peek() const {return begin() + reader_index_;}

    const char* find_crtf() const {
        const char* crlf = std::search(peek(),begin_write(),CRLF,CRLF+2);
        return crlf == begin_write()? nullptr : crlf;
    }

    const char*find_eol() const {
        const void* eol = memchr(peek(),'\n',readable_bytes());
        return static_cast<const char*>(eol);
    }

    void retrieve(size_t len) {
        if (len < readable_bytes()) {
            reader_index_ += len;
        }else {
            retrieve_all();
        }
    }

    void retrieve_until(const char* end) {
        retrieve(end - peek());
    }

    void retrieve_all() {
        reader_index_ = 0;
        writer_index_ = 0;
    }

    std::string retrieve_as_string(size_t len) {
        std::string result(peek(),len);
        retrieve(len);
        return result;
    }

    std::string retrieve_all_as_string() {
        return retrieve_as_string(readable_bytes());
    }

    void append(const char* data, size_t len) {
        ensure_writable_bytes(len);
        std::copy(data, data + len, begin_write());
        writer_index_ += len;
    }

    void append(const std::string& data) {
        append(data.data(), data.size());
    }

    void ensure_writable_bytes(size_t len) {
        if (writable_bytes() < len) {
            make_space(len);
        }
    }

    void has_written(size_t len) {writer_index_ += len;}

    void shrink_if_needed() {
        if (buffer_.size() > HIGH_WATER_MARK && readable_bytes() < LOW_WATER_MARK) {
            buffer_.shrink_to_fit();
        }
    }

    ssize_t read_fd(int fd,int *saved_errno);

private:
    static constexpr char CRLF[] = "\r\n";

    std::vector<char> buffer_;
    size_t reader_index_;
    size_t writer_index_;

    char* begin() {return buffer_.data();}
    const char* begin() const {return buffer_.data();}

    void make_space(size_t len) {
        if (writable_bytes() + prependable_bytes() < len) {
            size_t new_size = std::max(buffer_.size()*2,writer_index_ + len);
            if (new_size > MAX_BUFFER_SIZE) {
                throw std::length_error("Buffer overflow: exceed MAX_BUFFER_SIZE");
            }
            buffer_.resize(new_size);
        }else {
            size_t readable = readable_bytes();
            std::copy(begin()+reader_index_,begin()+writer_index_,begin());

            reader_index_ = 0;
            writer_index_ = readable;
        }
    }
};