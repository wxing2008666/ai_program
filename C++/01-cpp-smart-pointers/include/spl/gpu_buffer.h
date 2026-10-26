#ifndef SPL_GPU_BUFFER_H
#define SPL_GPU_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "spl/intrusive_ptr.h"
#include "spl/tracked.h"

namespace spl {

struct GpuBufferTag {};

class GpuBuffer : public RefCounted, public Tracked<GpuBufferTag> {
public:
    GpuBuffer(std::size_t bytes, int device);
    ~GpuBuffer();

    std::uint8_t* data();
    const std::uint8_t* data() const;
    std::size_t size() const;
    int device() const;

private:
    GpuBuffer(const GpuBuffer&);
    GpuBuffer& operator=(const GpuBuffer&);

    std::unique_ptr<std::uint8_t[]> data_;
    std::size_t bytes_;
    int device_;
};

intrusive_ptr<GpuBuffer> make_gpu_buffer(std::size_t bytes, int device);

}

#endif
