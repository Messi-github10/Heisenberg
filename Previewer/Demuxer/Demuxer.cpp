//
// Created by NiceFold on 2026/6/21.
//

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
}

#include <Demuxer/Demuxer.hpp>

#include <cstring>

namespace heisenberg {
namespace demuxer {

Demuxer::Demuxer() = default;

Demuxer::~Demuxer() {
    close();
}

int Demuxer::open(const std::string &url) {
    close();

    AVFormatContext *ctx = nullptr;
    int ret = avformat_open_input(&ctx, url.c_str(), nullptr, nullptr);
    if (ret < 0) {
        return ret;
    }

    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0) {
        avformat_close_input(&ctx);
        return ret;
    }

    formatCtx_ = ctx;
    url_ = url;

    ret = buildStreams();
    if (ret < 0) {
        avformat_close_input(&formatCtx_);
        url_.clear();
        return ret;
    }

    return 0;
}

void Demuxer::close() {
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    url_.clear();
    streams_.clear();
}

std::shared_ptr<Packet> Demuxer::readPacket() {
    if (!formatCtx_) {
        return nullptr;
    }

    AVPacket avpkt = {};
    int ret = av_read_frame(formatCtx_, &avpkt);
    if (ret < 0) {
        av_packet_unref(&avpkt);
        return nullptr;
    }

    if (!findStream(avpkt.stream_index)) {
        av_packet_unref(&avpkt);
        return nullptr;
    }

    auto packet = avpacketToPacket(&avpkt);
    av_packet_unref(&avpkt);

    return packet;
}

int Demuxer::seek(double seconds, int streamIndex, int flags) {
    if (!formatCtx_) {
        return -1;
    }

    int64_t seekTarget = static_cast<int64_t>(seconds * AV_TIME_BASE);
    return av_seek_frame(formatCtx_, streamIndex, seekTarget, flags);
}

double Demuxer::duration() const {
    if (!formatCtx_ || formatCtx_->duration == AV_NOPTS_VALUE) {
        return -1.0;
    }
    return static_cast<double>(formatCtx_->duration) / AV_TIME_BASE;
}

bool Demuxer::seekable() const {
    if (!formatCtx_ || !formatCtx_->pb) {
        return false;
    }
    return (formatCtx_->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
}

double Demuxer::startTime() const {
    if (!formatCtx_ || formatCtx_->start_time == AV_NOPTS_VALUE) {
        return 0.0;
    }
    return static_cast<double>(formatCtx_->start_time) / AV_TIME_BASE;
}

std::string Demuxer::formatName() const {
    if (!formatCtx_ || !formatCtx_->iformat || !formatCtx_->iformat->name) {
        return {};
    }
    return formatCtx_->iformat->name;
}

CodecParams::ID Demuxer::mapCodecId(AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_H264:   return CodecParams::H264;
    case AV_CODEC_ID_HEVC:   return CodecParams::HEVC;
    case AV_CODEC_ID_VP9:    return CodecParams::VP9;
    case AV_CODEC_ID_AV1:    return CodecParams::AV1;
    case AV_CODEC_ID_AAC:    return CodecParams::AAC;
    case AV_CODEC_ID_MP3:    return CodecParams::MP3;
    case AV_CODEC_ID_OPUS:   return CodecParams::OPUS;
    case AV_CODEC_ID_FLAC:   return CodecParams::FLAC;
    case AV_CODEC_ID_VORBIS: return CodecParams::VORBIS;
    default:                 return CodecParams::UNKNOWN;
    }
}

CodecParams Demuxer::avstreamToCodecparams(const AVStream *avstream) {
    CodecParams params;
    AVCodecParameters *codecPar = avstream->codecpar;

    if (codecPar->codec_type == AVMEDIA_TYPE_VIDEO) {
        params.type = CodecParams::VIDEO;
    } else if (codecPar->codec_type == AVMEDIA_TYPE_AUDIO) {
        params.type = CodecParams::AUDIO;
    }

    params.codecId = mapCodecId(codecPar->codec_id);

    if (codecPar->extradata && codecPar->extradata_size > 0) {
        params.setExtradata(codecPar->extradata,
                            static_cast<size_t>(codecPar->extradata_size));
    }

    params.width = codecPar->width;
    params.height = codecPar->height;

    AVRational fps = avstream->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) {
        fps = avstream->r_frame_rate;
    }
    params.fpsNum = fps.num;
    params.fpsDen = (fps.den > 0) ? fps.den : 1;

    AVRational sar = codecPar->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
        sar.num = 1;
        sar.den = 1;
    }
    params.sarNum = sar.num;
    params.sarDen = sar.den;

    params.sampleRate = codecPar->sample_rate;
    params.channels = codecPar->ch_layout.nb_channels;
    params.bitDepth = codecPar->bits_per_raw_sample;
    params.blockAlign = codecPar->block_align;

    params.tbNum = avstream->time_base.num;
    params.tbDen = avstream->time_base.den;

    return params;
}

int Demuxer::buildStreams() {
    streams_.clear();

    for (unsigned i = 0; i < formatCtx_->nb_streams; i++) {
        AVStream *avs = formatCtx_->streams[i];
        AVMediaType avType = avs->codecpar->codec_type;

        if (avType != AVMEDIA_TYPE_VIDEO && avType != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        Stream s;
        s.index = static_cast<int>(i);
        s.demuxerId = avs->id;
        s.type = (avType == AVMEDIA_TYPE_VIDEO) ? Stream::VIDEO : Stream::AUDIO;
        s.codec = avstreamToCodecparams(avs);

        AVDictionaryEntry *langEntry =
            av_dict_get(avs->metadata, "language", nullptr, 0);
        if (langEntry) {
            s.language = langEntry->value;
        }

        AVDictionaryEntry *titleEntry =
            av_dict_get(avs->metadata, "title", nullptr, 0);
        if (titleEntry) {
            s.title = titleEntry->value;
        }

        s.isDefault = (avs->disposition & AV_DISPOSITION_DEFAULT) != 0;
        s.isForced = (avs->disposition & AV_DISPOSITION_FORCED) != 0;

        streams_.push_back(std::move(s));
    }

    return 0;
}

std::shared_ptr<Packet> Demuxer::avpacketToPacket(const AVPacket *avpkt) {
    auto packet = std::make_shared<Packet>(static_cast<size_t>(avpkt->size));

    if (avpkt->data && avpkt->size > 0) {
        std::memcpy(packet->data(), avpkt->data, static_cast<size_t>(avpkt->size));
    }

    AVStream *avs = formatCtx_->streams[avpkt->stream_index];
    double timeBase = av_q2d(avs->time_base);

    packet->pts =
        (avpkt->pts == AV_NOPTS_VALUE) ? -1.0 : avpkt->pts * timeBase;
    packet->dts =
        (avpkt->dts == AV_NOPTS_VALUE) ? -1.0 : avpkt->dts * timeBase;
    packet->duration = avpkt->duration * timeBase;

    packet->streamIndex = avpkt->stream_index;
    packet->keyframe = (avpkt->flags & AV_PKT_FLAG_KEY) != 0;
    packet->filePos = avpkt->pos;

    return packet;
}

const Stream *Demuxer::findStream(int streamIndex) const {
    for (const auto &s : streams_) {
        if (s.index == streamIndex) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace demuxer
} // namespace heisenberg
