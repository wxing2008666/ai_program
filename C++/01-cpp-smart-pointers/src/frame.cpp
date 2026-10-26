#include "spl/frame.h"

#include <stdexcept>

namespace spl {
namespace {

std::unique_ptr<std::uint8_t[]> allocate_frame_bytes(std::size_t bytes) {
    if (bytes == 0) {
        throw std::invalid_argument("FrameBuffer size must be greater than zero");
    }
    return std::unique_ptr<std::uint8_t[]>(new std::uint8_t[bytes]());
}

}

FrameBuffer::FrameBuffer(std::size_t bytes)
    : data_(allocate_frame_bytes(bytes)), size_(bytes) {}

FrameBuffer::~FrameBuffer() {}

FrameBufferPtr make_frame_buffer(std::size_t bytes) {
    return std::make_shared<FrameBuffer>(bytes);
}

Frame Frame::wrap(FrameBufferPtr buffer, int width, int height, int stride) {
    if (!buffer) {
        throw std::invalid_argument("Frame::wrap requires a buffer");
    }
    if (width <= 0 || height <= 0 || stride < width) {
        throw std::invalid_argument("Frame::wrap received invalid geometry");
    }
    const std::size_t required =
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    if (buffer->size() < required) {
        throw std::invalid_argument("Frame::wrap buffer is too small");
    }
    const std::uint8_t* base = buffer->data();
    std::shared_ptr<const std::uint8_t> pixels(buffer, base);
    return Frame(std::move(pixels), width, height, stride);
}

Frame Frame::wrap(FrameBufferPtr buffer, int width, int height) {
    return wrap(std::move(buffer), width, height, width);
}

std::uint8_t Frame::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        throw std::out_of_range("Frame::at coordinates are outside the frame");
    }
    return data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_) +
                  static_cast<std::size_t>(x)];
}

Frame Frame::slice(int x, int y, int width, int height) const {
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > width_ || y + height > height_) {
        throw std::out_of_range("Frame::slice region is outside the frame");
    }
    const std::uint8_t* base =
        pixels_.get() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_) +
        static_cast<std::size_t>(x);
    std::shared_ptr<const std::uint8_t> pixels(pixels_, base);
    return Frame(std::move(pixels), width, height, stride_);
}

}
