#include "spl/plugin.h"

#include <sstream>
#include <stdexcept>

namespace spl {

Plugin::~Plugin() {}

std::string InvertPlugin::name() const {
    return "invert";
}

Frame InvertPlugin::apply(const Frame& input) const {
    if (!input) {
        throw std::invalid_argument("InvertPlugin::apply requires a valid frame");
    }
    const std::size_t bytes =
        static_cast<std::size_t>(input.stride()) * static_cast<std::size_t>(input.height());
    FrameBufferPtr output = make_frame_buffer(bytes);
    for (int y = 0; y < input.height(); ++y) {
        const std::uint8_t* source =
            input.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(input.stride());
        std::uint8_t* destination =
            output->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(input.stride());
        for (int x = 0; x < input.width(); ++x) {
            destination[x] = static_cast<std::uint8_t>(255 - source[x]);
        }
    }
    return Frame::wrap(output, input.width(), input.height(), input.stride());
}

ThresholdPlugin::ThresholdPlugin(int threshold) : threshold_(threshold) {
    if (threshold < 0 || threshold > 255) {
        throw std::invalid_argument("ThresholdPlugin threshold must be in [0, 255]");
    }
}

std::string ThresholdPlugin::name() const {
    std::ostringstream stream;
    stream << "threshold:" << threshold_;
    return stream.str();
}

Frame ThresholdPlugin::apply(const Frame& input) const {
    if (!input) {
        throw std::invalid_argument("ThresholdPlugin::apply requires a valid frame");
    }
    const std::size_t bytes =
        static_cast<std::size_t>(input.stride()) * static_cast<std::size_t>(input.height());
    FrameBufferPtr output = make_frame_buffer(bytes);
    for (int y = 0; y < input.height(); ++y) {
        const std::uint8_t* source =
            input.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(input.stride());
        std::uint8_t* destination =
            output->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(input.stride());
        for (int x = 0; x < input.width(); ++x) {
            destination[x] = source[x] > threshold_ ? 255 : 0;
        }
    }
    return Frame::wrap(output, input.width(), input.height(), input.stride());
}

std::string PassthroughPlugin::name() const {
    return "passthrough";
}

Frame PassthroughPlugin::apply(const Frame& input) const {
    if (!input) {
        throw std::invalid_argument("PassthroughPlugin::apply requires a valid frame");
    }
    return input;
}

}
