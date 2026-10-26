#include <stdexcept>
#include <string>
#include <utility>

#include "spl/codec.h"
#include "test_framework.h"

namespace {

spl::Frame make_codec_frame() {
    spl::FrameBufferPtr buffer = spl::make_frame_buffer(4 * 4);
    return spl::Frame::wrap(buffer, 4, 4);
}

}

SPL_TEST(decoder_pimpl_move_and_validity) {
    spl::Decoder decoder("h264", 80);
    SPL_CHECK(decoder.valid());
    SPL_CHECK_EQ(decoder.describe(), std::string("h264/q80"));

    spl::Frame input = make_codec_frame();
    spl::Frame output = decoder.decode(input);
    SPL_CHECK(output.shares_owner_with(input));

    spl::Decoder moved(std::move(decoder));
    SPL_CHECK(!decoder.valid());
    SPL_CHECK(moved.valid());
    SPL_CHECK_EQ(decoder.describe(), std::string("<moved-from>"));
    SPL_CHECK_EQ(moved.describe(), std::string("h264/q80"));
    SPL_CHECK_THROW(decoder.decode(input), std::logic_error);

    spl::Decoder assigned("raw", 100);
    assigned = std::move(moved);
    SPL_CHECK(assigned.valid());
    SPL_CHECK_EQ(assigned.describe(), std::string("h264/q80"));
}

SPL_TEST(decoder_rejects_invalid_configuration) {
    SPL_CHECK_THROW(spl::Decoder decoder("", 80), std::invalid_argument);
    SPL_CHECK_THROW(spl::Decoder decoder("h264", -1), std::invalid_argument);
    SPL_CHECK_THROW(spl::Decoder decoder("h264", 101), std::invalid_argument);
}
