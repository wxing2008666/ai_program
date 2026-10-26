#ifndef SPL_FRAME_H
#define SPL_FRAME_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "spl/tracked.h"

namespace spl {

struct FrameBufferTag {};

class FrameBuffer : public Tracked<FrameBufferTag> {
public:
    explicit FrameBuffer(std::size_t bytes);
    ~FrameBuffer();

    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    std::uint8_t* data() {
        return data_.get();
    }

    const std::uint8_t* data() const {
        return data_.get();
    }

    std::size_t size() const {
        return size_;
    }

private:
    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t size_;
};

typedef std::shared_ptr<FrameBuffer> FrameBufferPtr;

FrameBufferPtr make_frame_buffer(std::size_t bytes);

class Frame {
public:
    typedef std::shared_ptr<const void> OwnerKey;

    Frame() : width_(0), height_(0), stride_(0) {}

    Frame(std::shared_ptr<const std::uint8_t> pixels,
          int width,
          int height,
          int stride) noexcept
        : pixels_(std::move(pixels)),
          width_(width),
          height_(height),
          stride_(stride) {}

    static Frame wrap(FrameBufferPtr buffer, int width, int height, int stride);
    static Frame wrap(FrameBufferPtr buffer, int width, int height);

    const std::uint8_t* data() const {
        return pixels_.get();
    }

    int width() const {
        return width_;
    }

    int height() const {
        return height_;
    }

    int stride() const {
        return stride_;
    }

    std::uint8_t at(int x, int y) const;

    Frame slice(int x, int y, int width, int height) const;

    OwnerKey owner_key() const {
        return OwnerKey(pixels_, static_cast<const void*>(0));
    }

    long owner_use_count() const {
        return pixels_.use_count();
    }

    bool shares_owner_with(const Frame& other) const {
        return !pixels_.owner_before(other.pixels_) &&
               !other.pixels_.owner_before(pixels_);
    }

    explicit operator bool() const {
        return static_cast<bool>(pixels_);
    }

private:
    std::shared_ptr<const std::uint8_t> pixels_;
    int width_;
    int height_;
    int stride_;
};

}

#endif
