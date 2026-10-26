#ifndef SPL_CODEC_H
#define SPL_CODEC_H

#include <memory>
#include <string>

#include "spl/frame.h"

namespace spl {

class Decoder {
public:
    Decoder(const std::string& codec, int quality);
    ~Decoder();

    Decoder(Decoder&& other) noexcept;
    Decoder& operator=(Decoder&& other) noexcept;

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    std::string describe() const;
    Frame decode(const Frame& input) const;
    bool valid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
