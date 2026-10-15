#include "Encoding.hpp"
#include "EncoderInternal.hpp"

namespace heisenberg {
using namespace encoding_detail;

struct EncodedPacket::Impl {
    PacketPtr packet;
    AVRational timeBase;
    Impl(const AVPacket& source, AVRational tb) : packet(encoding_detail::packet()), timeBase(tb) {
        encoding_detail::timeBase(tb);
        check(av_packet_ref(packet.get(), &source), "av_packet_ref");
        packet->time_base = tb;
    }
};
EncodedPacket::EncodedPacket(const AVPacket& source, AVRational tb)
    : impl_(std::make_unique<Impl>(source, tb)) {}
EncodedPacket::~EncodedPacket() = default;
EncodedPacket::EncodedPacket(EncodedPacket&&) noexcept = default;
EncodedPacket& EncodedPacket::operator=(EncodedPacket&&) noexcept = default;
const AVPacket* EncodedPacket::native() const noexcept { return impl_ ? impl_->packet.get() : nullptr; }
AVRational EncodedPacket::timeBase() const noexcept { return impl_ ? impl_->timeBase : AVRational{0, 1}; }

struct CodecParameters::Impl {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    Impl() { if (!parameters) throw std::bad_alloc(); }
    ~Impl() { avcodec_parameters_free(&parameters); }
};
CodecParameters::CodecParameters(const AVCodecContext& context) : impl_(std::make_unique<Impl>()) {
    check(avcodec_parameters_from_context(impl_->parameters, &context), "avcodec_parameters_from_context");
}
CodecParameters::CodecParameters(const AVCodecParameters& parameters) : impl_(std::make_unique<Impl>()) {
    check(avcodec_parameters_copy(impl_->parameters, &parameters), "avcodec_parameters_copy");
}
CodecParameters::~CodecParameters() = default;
CodecParameters::CodecParameters(const CodecParameters& other) : impl_(std::make_unique<Impl>()) {
    if (!other.native()) throw EncodingError("copy of moved-from codec parameters");
    check(avcodec_parameters_copy(impl_->parameters, other.native()), "avcodec_parameters_copy");
}
CodecParameters& CodecParameters::operator=(const CodecParameters& other) {
    if (this != &other) { CodecParameters copy(other); impl_.swap(copy.impl_); }
    return *this;
}
CodecParameters::CodecParameters(CodecParameters&&) noexcept = default;
CodecParameters& CodecParameters::operator=(CodecParameters&&) noexcept = default;
const AVCodecParameters* CodecParameters::native() const noexcept { return impl_ ? impl_->parameters : nullptr; }

} // namespace heisenberg
