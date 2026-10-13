//
// Created by NiceFold on 2026/10/13.
//

#include "MultiMedia/MediaInfo/MediaInfoProbe.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <MediaInfoDLL/MediaInfoDLL.h>

namespace heisenberg::mediainfo {
namespace {

std::string utf8FromWide(const wchar_t* text) {
    if (!text || !*text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string utf8(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring wideFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    return wide;
}

void addField(StreamInfo& stream,
              MediaInfoDLL::MediaInfo& info,
              MediaInfoDLL::stream_t kind,
              size_t index,
              const char* name) {
    const std::wstring wideName = wideFromUtf8(name);
    const std::string value = utf8FromWide(info.Get(kind, index, wideName).c_str());
    if (!value.empty()) stream.fields.push_back(Field{name, value});
}

StreamInfo readStream(MediaInfoDLL::MediaInfo& info,
                      MediaInfoDLL::stream_t kind,
                      StreamKind streamKind,
                      size_t index,
                      std::initializer_list<const char*> names) {
    StreamInfo stream;
    stream.kind = streamKind;
    for (const char* name : names)
        addField(stream, info, kind, index, name);
    return stream;
}

} // namespace

ProbeResult probeFile(const std::string& path) {
    ProbeResult result;
    MediaInfoDLL::MediaInfo info;
    if (!info.IsReady()) {
        result.error = "MediaInfo.dll could not be loaded.";
        return result;
    }
    info.Option(__T("Internet"), __T("No"));
    if (!info.Open(wideFromUtf8(path))) {
        result.error = "Could not read this file.";
        return result;
    }

    result.streams.push_back(readStream(
        info, MediaInfoDLL::Stream_General, StreamKind::General, 0,
        {"Format", "FileSize", "Duration", "OverallBitRate"}));

    const size_t videoCount = info.Count_Get(MediaInfoDLL::Stream_Video);
    for (size_t index = 0; index < videoCount; ++index) {
        auto stream = readStream(
            info, MediaInfoDLL::Stream_Video, StreamKind::Video, index,
            {"Format", "Format_Profile", "Width", "Height", "FrameRate",
             "BitRate", "BitDepth", "ColorSpace", "ChromaSubsampling"});
        if (!stream.fields.empty()) result.streams.push_back(std::move(stream));
    }

    const size_t audioCount = info.Count_Get(MediaInfoDLL::Stream_Audio);
    for (size_t index = 0; index < audioCount; ++index) {
        auto stream = readStream(
            info, MediaInfoDLL::Stream_Audio, StreamKind::Audio, index,
            {"Format", "Format_Profile", "Channels", "SamplingRate", "BitRate", "Language"});
        if (!stream.fields.empty()) result.streams.push_back(std::move(stream));
    }

    info.Close();
    result.loaded = true;
    return result;
}

} // namespace heisenberg::mediainfo
