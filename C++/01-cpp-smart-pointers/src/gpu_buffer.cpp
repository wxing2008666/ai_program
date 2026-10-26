#include "spl/gpu_buffer.h"

#include <cstring>
#include <stdexcept>

namespace spl {

GpuBuffer::GpuBuffer(std::size_t bytes, int device)
    : data_(new std::uint8_t[bytes]()),
      bytes_(bytes),
      device_(device) {
    if (bytes == 0) {
        throw std::invalid_argument("GpuBuffer size must be greater than zero");
    }
}

GpuBuffer::~GpuBuffer() {}

std::uint8_t* GpuBuffer::data() {
    return data_.get();
}

const std::uint8_t* GpuBuffer::data() const {
    return data_.get();
}

std::size_t GpuBuffer::size() const {
    return bytes_;
}

int GpuBuffer::device() const {
    return device_;
}

intrusive_ptr<GpuBuffer> make_gpu_buffer(std::size_t bytes, int device) {
    if (bytes == 0) {
        throw std::invalid_argument("GpuBuffer size must be greater than zero");
    }
    return make_intrusive<GpuBuffer>(bytes, device);
}

}
