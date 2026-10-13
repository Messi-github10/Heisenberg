#include "TimelineProducer.hpp"

#include <Compositing/ProfileCanvas.hpp>
#include <Models/MediaResolver.hpp>
#include <Preview/Producer.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace heisenberg {

struct TimelineProducer::Impl {
    Timeline timeline;
    MediaManifest manifest;
    std::string projectDir;
    Profile profile;
    std::string resource = "<timeline>";
    bool hardwareDecode = false;
    int64_t position = 0;
    ProfileCanvas canvas;
    std::unordered_map<std::string, std::unique_ptr<Producer>> producers;
    std::vector<std::string> warnings;

    Impl(Timeline value, MediaManifest media, std::string dir)
        : timeline(std::move(value)), manifest(std::move(media)), projectDir(std::move(dir)),
          profile(Profile::fromTimelineSettings(timeline.width, timeline.height, timeline.fps)) {
        canvas.setProfile(profile);
    }

    const Track* track() const { return timeline.tracks.empty() ? nullptr : &timeline.tracks.front(); }
};

TimelineProducer::TimelineProducer(Timeline timeline, MediaManifest manifest, std::string projectDir)
    : impl_(std::make_unique<Impl>(std::move(timeline), std::move(manifest), std::move(projectDir))) {}
TimelineProducer::~TimelineProducer() = default;
TimelineProducer::TimelineProducer(TimelineProducer&&) noexcept = default;
TimelineProducer& TimelineProducer::operator=(TimelineProducer&&) noexcept = default;

void TimelineProducer::setHardwareDecode(bool enabled) {
    impl_->hardwareDecode = enabled;
    impl_->canvas.setHardwareDecode(enabled);
    for (auto& [id, producer] : impl_->producers) producer->setHardwareDecode(enabled);
}

bool TimelineProducer::prepare(std::string* error) {
    const ProjectCompatibility compatibility = compatibilityFor(impl_->timeline);
    if (compatibility.level == CompatibilityLevel::Unsupported) {
        if (error) *error = compatibility.messages.empty() ? "unsupported timeline" : compatibility.messages.front();
        return false;
    }
    impl_->warnings = compatibility.messages;
    return true;
}

bool TimelineProducer::open(std::string* error) {
    const ProjectCompatibility compatibility = compatibilityFor(impl_->timeline);
    if (compatibility.level == CompatibilityLevel::Unsupported) {
        if (error) *error = compatibility.messages.empty()
            ? "unsupported timeline" : compatibility.messages.front();
        return false;
    }
    impl_->producers.clear();
    const Track* track = impl_->track();
    if (!track) return true;
    MediaResolver resolver(impl_->manifest, impl_->projectDir);
    for (const Clip& clip : track->clips) {
        if (impl_->producers.count(clip.mediaRef)) continue;
        const std::string path = resolver.resolvePath(clip.mediaRef);
        if (path.empty()) {
            impl_->warnings.push_back("offline media: " + clip.mediaRef);
            continue;
        }
        auto producer = std::make_unique<Producer>(impl_->profile);
        producer->setHardwareDecode(impl_->hardwareDecode);
        std::string openError;
        if (!producer->open(path, &openError)) {
            impl_->warnings.push_back("unavailable media: " + clip.mediaRef + " (" + openError + ")");
            continue;
        }
        impl_->producers.emplace(clip.mediaRef, std::move(producer));
    }
    return true;
}

const std::vector<std::string>& TimelineProducer::warnings() const { return impl_->warnings; }
const Profile& TimelineProducer::profile() const { return impl_->profile; }
const std::string& TimelineProducer::resource() const { return impl_->resource; }
int64_t TimelineProducer::in() const { return 0; }
int64_t TimelineProducer::out() const { return length() > 0 ? length() - 1 : 0; }
int64_t TimelineProducer::length() const { return impl_->timeline.totalFrames(); }
int64_t TimelineProducer::position() const { return impl_->position; }

bool TimelineProducer::seekable() const {
    for (const auto& [id, producer] : impl_->producers)
        if (!producer || !producer->seekable()) return false;
    return length() > 0;
}

bool TimelineProducer::seek(int64_t frame) {
    if (length() <= 0) return false;
    impl_->position = std::clamp(frame, int64_t{0}, length() - 1);
    return true;
}

ProducerFrame TimelineProducer::getFrame(int64_t frameNumber) {
    ProducerFrame frame;
    if (frameNumber < 0 || frameNumber >= length()) {
        frame.eof = true;
        frame.position = length() > 0 ? std::clamp(frameNumber, int64_t{0}, length() - 1) : 0;
        impl_->position = frame.position;
        return frame;
    }
    const Track* track = impl_->track();
    const Clip* clip = track ? track->clipAt(frameNumber) : nullptr;
    if (!clip) {
        frame = impl_->canvas.frame(frameNumber, CanvasColor::Black);
        impl_->position = frameNumber;
        return frame;
    }
    const auto found = impl_->producers.find(clip->mediaRef);
    if (found == impl_->producers.end()) {
        frame = impl_->canvas.frame(frameNumber, CanvasColor::Black);
        impl_->position = frameNumber;
        return frame;
    }
    const int64_t relative = frameNumber - clip->startFrame;
    const int64_t source = clip->trimStartFrame + static_cast<int64_t>(std::llround(relative * clip->speed));
    frame = found->second->getFrame(source);
    if (frame.eof) {
        frame = impl_->canvas.frame(frameNumber, CanvasColor::Black);
    } else {
        ProducerFrame fallback;
        if (!frame.hasVideo() || !frame.hasAudio() || track->hidden || track->muted)
            fallback = impl_->canvas.frame(frameNumber, CanvasColor::Black);
        if (!frame.hasVideo() || track->hidden) frame.video = std::move(fallback.video);
        if (!frame.hasAudio() || track->muted) frame.audio = std::move(fallback.audio);
    }
    frame.position = frameNumber;
    impl_->position = frameNumber;
    return frame;
}

} // namespace heisenberg
