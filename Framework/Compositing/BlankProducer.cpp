#include "BlankProducer.hpp"

#include "ProfileCanvas.hpp"

#include <algorithm>
#include <memory>

namespace heisenberg {

struct BlankProducer::Impl {
    ProfileCanvas canvas;
    std::string resource = kBlankResource;
    int64_t length = 0;
    int64_t position = 0;

    int64_t clampPosition(int64_t value) const {
        if (length <= 0) return 0;
        return std::clamp(value, int64_t{0}, length - 1);
    }
};

BlankProducer::BlankProducer(Profile profile, int64_t length)
    : impl_(std::make_unique<Impl>()) {
    impl_->canvas.setProfile(std::move(profile));
    ensureLength(length);
}

BlankProducer::~BlankProducer() = default;

void BlankProducer::setHardwareDecode(bool enabled) {
    impl_->canvas.setHardwareDecode(enabled);
}

void BlankProducer::ensureLength(int64_t length) {
    if (length > impl_->length) impl_->length = length;
}

const Profile& BlankProducer::profile() const {
    return impl_->canvas.profile();
}

const std::string& BlankProducer::resource() const {
    return impl_->resource;
}

int64_t BlankProducer::in() const {
    return 0;
}

int64_t BlankProducer::out() const {
    return impl_->length > 0 ? impl_->length - 1 : 0;
}

int64_t BlankProducer::length() const {
    return impl_->length;
}

int64_t BlankProducer::position() const {
    return impl_->position;
}

bool BlankProducer::seekable() const {
    return true;
}

bool BlankProducer::seek(int64_t position) {
    if (impl_->length <= 0) return false;
    impl_->position = impl_->clampPosition(position);
    return true;
}

ProducerFrame BlankProducer::getFrame(int64_t position) {
    ProducerFrame frame;
    if (impl_->length <= 0) {
        frame.eof = true;
        return frame;
    }

    const int64_t clamped = impl_->clampPosition(position);
    if (position < 0 || position >= impl_->length) {
        frame.eof = true;
        frame.position = clamped;
        impl_->position = clamped;
        return frame;
    }

    frame = impl_->canvas.frame(clamped, CanvasColor::White);
    impl_->position = clamped;
    return frame;
}

bool BlankProducer::isBlank() const {
    return true;
}

} // namespace heisenberg
