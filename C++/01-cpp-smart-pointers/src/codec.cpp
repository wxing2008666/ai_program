#include "spl/codec.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace spl {

struct Decoder::Impl {
    Impl(const std::string& codec_name, int quality_level)
        : codec(codec_name), quality(quality_level), decode_calls(0) {}

    std::string codec;
    int quality;
    mutable std::size_t decode_calls;
};

Decoder::Decoder(const std::string& codec, int quality)
    : impl_(new Impl(codec, quality)) {
    if (codec.empty()) {
        throw std::invalid_argument("Decoder codec name must not be empty");
    }
    if (quality < 0 || quality > 100) {
        throw std::invalid_argument("Decoder quality must be in [0, 100]");
    }
}

Decoder::~Decoder() {}

Decoder::Decoder(Decoder&& other) noexcept : impl_(std::move(other.impl_)) {}

Decoder& Decoder::operator=(Decoder&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

std::string Decoder::describe() const {
    if (!impl_) {
        return "<moved-from>";
    }
    std::ostringstream stream;
    stream << impl_->codec << "/q" << impl_->quality;
    return stream.str();
}

Frame Decoder::decode(const Frame& input) const {
    if (!impl_) {
        throw std::logic_error("Decoder::decode called on a moved-from decoder");
    }
    if (!input) {
        throw std::invalid_argument("Decoder::decode requires a valid frame");
    }
    ++impl_->decode_calls;
    return input;
}

bool Decoder::valid() const {
    return impl_.get() != 0;
}

}
