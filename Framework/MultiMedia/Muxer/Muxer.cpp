#include "Muxer.hpp"
#include <MultiMedia/Video/Encoder/EncoderInternal.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/intreadwrite.h>
}

namespace heisenberg {
using namespace encoding_detail;
struct Muxer::Impl {
    AVFormatContext* context = nullptr;
    enum class State { Configuring, Open, Finished, Failed } state = State::Configuring;
    std::vector<int64_t> lastDts;
    struct Duration { int64_t value; AVRational timeBase; };
    std::vector<std::optional<Duration>> durations;
    explicit Impl(const std::string& format) {
        require(!format.empty(), "muxer format must not be empty");
        check(avformat_alloc_output_context2(&context, nullptr, format.c_str(), nullptr), "avformat_alloc_output_context2");
        if (!context) throw EncodingError("could not allocate output format context");
        // Do not erase AAC priming or B-frame DTS by shifting to zero. MOV's
        // edit list represents priming while the packet timeline stays intact.
        context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    }
    ~Impl() {
        if (context) {
            if (context->pb && !(context->oformat->flags & AVFMT_NOFILE)) avio_closep(&context->pb);
            avformat_free_context(context);
        }
    }
    AVStream* stream(int index) const {
        require(index >= 0 && static_cast<unsigned>(index) < context->nb_streams, "invalid muxer stream index");
        return context->streams[index];
    }
    void openState() const {
        if (state != State::Open) throw EncodingError("muxer is not open (or has failed/finished)");
    }
};
Muxer::Muxer(const std::string& format) : impl_(std::make_unique<Impl>(format)) {}
Muxer::~Muxer() = default;
int Muxer::addStream(const CodecParameters& parameters, AVRational tb) {
    if (impl_->state != Impl::State::Configuring) throw EncodingError("addStream must precede muxer open");
    encoding_detail::timeBase(tb);
    require(parameters.native() != nullptr, "codec parameters are moved-from");
    require(parameters.native()->codec_type == AVMEDIA_TYPE_VIDEO || parameters.native()->codec_type == AVMEDIA_TYPE_AUDIO,
            "muxer only accepts audio/video streams");
    auto* stream = avformat_new_stream(impl_->context, nullptr);
    if (!stream) { impl_->state = Impl::State::Failed; throw std::bad_alloc(); }
    try {
        check(avcodec_parameters_copy(stream->codecpar, parameters.native()), "avcodec_parameters_copy(muxer)");
        stream->time_base = tb;
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) stream->avg_frame_rate = av_inv_q(tb);
        // Preserve hvc1; let the container select tags for the other codecs.
        stream->codecpar->codec_tag = stream->codecpar->codec_id == AV_CODEC_ID_HEVC ? MKTAG('h','v','c','1') : 0;
        impl_->lastDts.push_back(AV_NOPTS_VALUE);
        impl_->durations.emplace_back(std::nullopt);
    } catch (...) { impl_->state = Impl::State::Failed; throw; }
    return stream->index;
}
void Muxer::open(const std::string& path) {
    if (impl_->state != Impl::State::Configuring) throw EncodingError("muxer open is single-use");
    require(!path.empty(), "output path must not be empty");
    require(impl_->context->nb_streams > 0, "muxer requires at least one stream");
    AVDictionary* options = nullptr;
    try {
        if (!(impl_->context->oformat->flags & AVFMT_NOFILE))
            check(avio_open(&impl_->context->pb, path.c_str(), AVIO_FLAG_WRITE), "avio_open(output)");
        const auto* name = impl_->context->oformat->name;
        if (std::strcmp(name, "mp4") == 0 || std::strcmp(name, "mov") == 0) {
            check(av_dict_set(&options, "use_editlist", "1", 0), "set MOV edit list");
            // Represent AAC duration at sample precision rather than rounding
            // edit-list/media duration to the MOV default millisecond scale.
            check(av_dict_set(&options, "movie_timescale", "48000", 0), "set MOV movie timescale");
        }
        check(avformat_write_header(impl_->context, &options), "avformat_write_header");
        if (av_dict_count(options)) throw EncodingError("muxer left unrecognized options");
        if (impl_->context->pb) check(impl_->context->pb->error, "write output header");
        impl_->state = Impl::State::Open;
    } catch (...) { av_dict_free(&options); impl_->state = Impl::State::Failed; throw; }
    av_dict_free(&options);
}
AVRational Muxer::streamTimeBase(int index) const { return impl_->stream(index)->time_base; }
void Muxer::setStreamDuration(int index, int64_t duration, AVRational tb) {
    impl_->openState();
    encoding_detail::timeBase(tb);
    require(duration >= 0, "negative stream duration");
    auto* stream = impl_->stream(index);
    const int64_t scaled = av_rescale_q(duration, tb, stream->time_base);
    require(scaled >= 0 && scaled != std::numeric_limits<int64_t>::max(), "stream duration overflow");
    impl_->durations[index] = Impl::Duration{duration, tb};
    stream->duration = scaled;
}
void Muxer::write(const EncodedPacket& source, int index) {
    impl_->openState();
    auto* stream = impl_->stream(index);
    require(source.native() != nullptr, "packet is moved-from");
    encoding_detail::timeBase(source.timeBase());
    auto copy = encoding_detail::packet();
    check(av_packet_ref(copy.get(), source.native()), "av_packet_ref(muxer)");
    av_packet_rescale_ts(copy.get(), source.timeBase(), stream->time_base);
    copy->time_base = stream->time_base;
    copy->stream_index = index;
    copy->pos = -1;
    require(copy->pts != AV_NOPTS_VALUE && copy->dts != AV_NOPTS_VALUE, "muxer requires known PTS and DTS");
    require(copy->duration >= 0, "negative packet duration");
    const int64_t last = impl_->lastDts[index];
    const bool nonstrict = impl_->context->oformat->flags & AVFMT_TS_NONSTRICT;
    require(last == AV_NOPTS_VALUE || copy->dts > last || (nonstrict && copy->dts == last), "non-monotonic muxer DTS");
    require(copy->pts >= copy->dts, "packet PTS precedes DTS");
    const int64_t dts = copy->dts;
    // Preserve the side data in the copied packet and expose final padding in
    // the stream too (not all containers support end skip packet metadata).
    size_t skipSize = 0;
    const auto* skip = av_packet_get_side_data(copy.get(), AV_PKT_DATA_SKIP_SAMPLES, &skipSize);
    if (skip && skipSize >= 10 && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        stream->codecpar->initial_padding = std::max(stream->codecpar->initial_padding, static_cast<int>(AV_RL32(skip)));
        stream->codecpar->trailing_padding = std::max(stream->codecpar->trailing_padding, static_cast<int>(AV_RL32(skip + 4)));
    }
    try {
        check(av_interleaved_write_frame(impl_->context, copy.get()), "av_interleaved_write_frame");
        if (impl_->context->pb) check(impl_->context->pb->error, "write output packet");
        impl_->lastDts[index] = dts;
    } catch (...) { impl_->state = Impl::State::Failed; throw; }
}
void Muxer::finish() {
    if (impl_->state == Impl::State::Finished) return;
    impl_->openState();
    int firstError = 0;
    const char* operation = "flush muxer packets";
    auto remember = [&](int result, const char* name) {
        if (result < 0 && firstError == 0) { firstError = result; operation = name; }
    };
    remember(av_interleaved_write_frame(impl_->context, nullptr), "flush muxer packets");
    for (unsigned i = 0; i < impl_->context->nb_streams; ++i) {
        if (const auto& duration = impl_->durations[i])
            impl_->context->streams[i]->duration = av_rescale_q(duration->value, duration->timeBase,
                                                              impl_->context->streams[i]->time_base);
    }
    remember(av_write_trailer(impl_->context), "av_write_trailer");
    if (impl_->context->pb && !(impl_->context->oformat->flags & AVFMT_NOFILE)) {
        avio_flush(impl_->context->pb);
        remember(impl_->context->pb->error, "flush output IO");
        remember(avio_closep(&impl_->context->pb), "avio_closep(output)");
    }
    impl_->state = firstError < 0 ? Impl::State::Failed : Impl::State::Finished;
    check(firstError, operation);
}
} // namespace heisenberg
