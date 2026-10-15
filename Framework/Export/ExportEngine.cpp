#include "ExportEngine.hpp"

#include <Compositing/CompositionBuilder.hpp>
#include <Compositing/CompositionExecutor.hpp>
#include <Compositing/Grade/GradeCompiler.hpp>
#include <MultiMedia/Audio/Encoder/AudioEncoder.hpp>
#include <MultiMedia/Muxer/Muxer.hpp>
#include <MultiMedia/Video/Encoder/VideoEncoder.hpp>
#include <MultiMedia/Video/Renderer/Color/ColorPipeline.hpp>
#include <MultiMedia/Video/Renderer/CompositionRenderer.hpp>
#include <Platform/D3D11/D3D11Context.hpp>
#include <Platform/GpuContext.hpp>
#include <Platform/Vulkan/VulkanContext.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <windows.h>

extern "C" {
#include <libavutil/frame.h>
}

namespace heisenberg {
namespace {
namespace fs = std::filesystem;

struct Cancelled {};
thread_local const ExportEngine* currentExportWorker = nullptr;
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

fs::path normalized(const fs::path& path) {
    return fs::weakly_canonical(fs::absolute(path)).lexically_normal();
}

bool sameFile(const fs::path& left, const fs::path& right) {
    std::error_code ec;
    if (fs::equivalent(left, right, ec) && !ec) return true;
    const auto a = normalized(left).wstring();
    const auto b = normalized(right).wstring();
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

class StagedOutput {
public:
    StagedOutput(fs::path target, bool overwrite) : target_(std::move(target)), overwrite_(overwrite) {
        if (!fs::is_directory(target_.parent_path()))
            throw std::runtime_error("Output directory does not exist");
        if (fs::exists(target_) && (!overwrite_ || !fs::is_regular_file(target_)))
            throw std::runtime_error("Output already exists or is not a regular file");
        for (int attempt = 0; attempt < 16; ++attempt) {
            staging_ = target_.parent_path() / (L".heisenberg-" + fs::path(makeUuid()).wstring() + L".partial");
            const HANDLE handle = CreateFileW(staging_.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); return; }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Cannot create export staging file: " + std::to_string(GetLastError()));
        }
        throw std::runtime_error("Cannot allocate a unique export staging file");
    }
    ~StagedOutput() {
        if (!committed_) { std::error_code ignored; fs::remove(staging_, ignored); }
    }
    std::string path() const { return utf8(staging_); }
    void commit() {
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite_ ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(staging_.c_str(), target_.c_str(), flags))
            throw std::runtime_error("Cannot commit export output: " + std::to_string(GetLastError()));
        committed_ = true;
    }
private:
    fs::path target_, staging_;
    bool overwrite_ = false, committed_ = false;
};

void validateAppearance(const Clip& clip) {
    if (clip.opacity != 1.0 || clip.transform != Transform{} || clip.crop != Crop{} ||
        clip.edgeRounding != 0.0 || clip.edgeSoftness != 0.0 || clip.opacityTrack ||
        clip.positionTrack || clip.scaleTrack || clip.rotationTrack || clip.cropTrack ||
        (clip.blendMode && *clip.blendMode != "normal"))
        throw std::runtime_error("Unsupported clip appearance: " + clip.id);
    if (clip.volumeTrack ||
        (clip.fadeInFrames && clip.fadeInInterpolation != Interpolation::Linear) ||
        (clip.fadeOutFrames && clip.fadeOutInterpolation != Interpolation::Linear))
        throw std::runtime_error("Unsupported clip audio automation: " + clip.id);
    grade::CompiledGrade compiled;
    std::string error;
    if (!grade::GradeCompiler::compile(clip, 0, compiled, &error))
        throw std::runtime_error("Unsupported clip grade: " + error);
}

void checkInputPaths(const ExportRequest& request, const CompositionPlan& plan, const fs::path& output) {
    for (const auto* clips : {&plan.clips, &plan.audioClips})
        for (const auto& clip : *clips)
            if (!clip.sourcePath.empty() && sameFile(output, fs::u8path(clip.sourcePath)))
                throw std::runtime_error("Export output cannot replace an input media file");
    if (!request.snapshot.projectDir().empty()) {
        const auto root = fs::u8path(request.snapshot.projectDir());
        if (sameFile(output, root / "project.json") || sameFile(output, root / "media.json"))
            throw std::runtime_error("Export output cannot replace project documents");
    }
}
} // namespace

struct ExportEngine::Impl {
    explicit Impl(ExportEngine& owner) : owner(owner) {}
    ExportEngine& owner;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::thread worker;
    std::atomic<bool> active{false}, cancelled{false};
    ExportProgress progress;
    ExportResult result;

    void checkCancel() const { if (cancelled.load(std::memory_order_acquire)) throw Cancelled{}; }
    void publish(ExportProgress value) {
        { std::lock_guard lock(mutex); progress = value; }
        if (owner.onProgress) { try { owner.onProgress(value); } catch (...) {} }
    }
    void complete(ExportResult value) {
        { std::lock_guard lock(mutex); result = value; progress.state = value.state;
          if (value.succeeded()) progress.fraction = 1.0; }
        if (value.succeeded()) {
            const auto current = snapshotProgress();
            if (owner.onProgress) { try { owner.onProgress(current); } catch (...) {} }
        }
        active.store(false, std::memory_order_release);
        if (owner.onFinished) { try { owner.onFinished(value); } catch (...) {} }
    }
    ExportProgress snapshotProgress() const { std::lock_guard lock(mutex); return progress; }

    void run(ExportRequest request) {
        struct WorkerScope {
            explicit WorkerScope(ExportEngine& engine) { currentExportWorker = &engine; }
            ~WorkerScope() { currentExportWorker = nullptr; }
        } scope(owner);
        ExportResult finished;
        finished.outputPath = request.outputPath;
        finished.stage = "prepare";
        try {
            checkCancel();
            CompositionPlan plan;
            std::string error;
            if (!CompositionBuilder::build(request.snapshot, plan, &error)) throw std::runtime_error(error);
            if (plan.frameCount <= 0) throw std::runtime_error("The active timeline is empty");
            if ((plan.settings.width() & 1) || (plan.settings.height() & 1))
                throw std::runtime_error("YUV420 output requires even width and height");
            if (request.crf < 0 || request.crf > 51) throw std::runtime_error("CRF must be between 0 and 51");
            if (!validateColorSettings(plan.colorSettings, &error)) throw std::runtime_error(error);
            if (!supportedColorSettings(plan.colorSettings) || plan.colorSettings.configId != renderer::color::kConfigId)
                throw std::runtime_error("Unsupported project color policy or configuration version");
            if (plan.colorSettings.exportOutput.display != renderer::color::kExportDisplay ||
                (plan.colorSettings.exportOutput.view != renderer::color::kColorimetricView &&
                 plan.colorSettings.exportOutput.view != renderer::color::kAcesOutputView))
                throw std::runtime_error("This encoder output requires an SDR Rec.709 colorimetric or ACES output view");
            for (const auto* clips : {&plan.clips, &plan.audioClips})
                for (const auto& clip : *clips) validateAppearance(clip.visualSnapshot);
            finished.warnings = plan.warnings;
            const fs::path output = normalized(fs::u8path(request.outputPath));
            checkInputPaths(request, plan, output);
            StagedOutput staged(output, request.overwrite);
            CompositionExecutor executor;
            CompositionExecutor::Context context{0, [this] { return cancelled.load(std::memory_order_acquire); }};
            const auto opened = executor.open(plan, {request.hardwareDecode, CompositionErrorPolicy::Strict}, context);
            if (!opened) { checkCancel(); throw std::runtime_error(opened.error); }
            const int64_t totalSamples = opened.value.audioSampleCount;
            publish({ExportState::Preparing, 0, plan.frameCount, 0, totalSamples, 0.0});

            finished.stage = "initialize rendering";
            auto& vk = renderer::VulkanContext::instance();
            vk.initLoader(); vk.createInstance(); vk.createDevice();
            auto& d3d = renderer::D3D11Context::instance();
            d3d.createDevice();
            renderer::VulkanResources resources{vk.vkInstance(), vk.physicalDevice(), vk.device(),
                vk.graphicsQueueFamily(), vk.graphicsQueue(), vk.getInstanceProcAddr()};
            resources.enabledFeatures = vk.enabledDeviceFeatures();
            resources.enabledExtensions = vk.enabledDeviceExtensions();
            renderer::GpuContext gpu(resources);
            renderer::CompositionRenderer renderer;
            if (!renderer.initialize(gpu.plGpu(), gpu.plVulkan(), d3d.device(), d3d.context(), &error))
                throw std::runtime_error(error);
            renderer.setCancellation([this] { return cancelled.load(std::memory_order_acquire); });
            if (request.filterGraph && !renderer.setFilterGraphSnapshot(*request.filterGraph, &error))
                throw std::runtime_error(error);
            renderer::color::ColorPipeline color;

            finished.stage = "initialize encoding";
            VideoEncoderSettings videoSettings;
            videoSettings.width = plan.settings.width(); videoSettings.height = plan.settings.height();
            videoSettings.frameRate = {plan.settings.frameRate().numerator(), plan.settings.frameRate().denominator()};
            videoSettings.sampleAspect = {plan.settings.sampleAspect().numerator(), plan.settings.sampleAspect().denominator()};
            videoSettings.codec = request.videoCodec == ExportVideoCodec::H264 ? VideoEncodingCodec::H264 : VideoEncodingCodec::H265;
            videoSettings.crf = request.crf; videoSettings.preset = request.preset;
            VideoEncoder video(videoSettings);
            AudioEncoder audio;
            Muxer muxer;
            const int videoStream = muxer.addStream(video.codecParameters(), video.timeBase());
            const int audioStream = muxer.addStream(audio.codecParameters(), audio.timeBase());
            muxer.open(staged.path());
            const auto write = [&muxer](EncodedPackets packets, int stream) {
                for (const auto& packet : packets) muxer.write(packet, stream);
            };
            int64_t nextSample = 0;
            for (int64_t frame = 0; frame < plan.frameCount; ++frame) {
                checkCancel(); finished.stage = "render";
                const auto decoded = executor.videoFrame(frame, context);
                if (!decoded) { checkCancel(); throw std::runtime_error(decoded.error); }
                const auto& sample = decoded.value;
                if (sample.source && !sample.blank) {
                    const auto assignment = plan.colorSettings.inputColorSpaces.find(sample.mediaRef);
                    const auto resolved = color.resolveInput(sample.source->spec(),
                        assignment == plan.colorSettings.inputColorSpaces.end() ? "" : assignment->second,
                        plan.colorSettings.inputPolicyVersion);
                    if (resolved.source == renderer::color::InputSelectionSource::Fallback)
                        throw std::runtime_error("Resolve input color space before export: " + sample.mediaRef);
                }
                auto image = renderer.render(plan, frame, sample.source, &error);
                if (!image) throw std::runtime_error(error);
                renderer::ColorOutputRequest target;
                target.display = plan.colorSettings.exportOutput.display;
                target.view = plan.colorSettings.exportOutput.view;
                auto encoded = renderer.output(image, target, &error);
                if (!encoded) throw std::runtime_error(error);
                auto pixels = renderer.download(encoded, renderer::DownloadFormat::RgbaFloat, &error);
                if (!pixels) throw std::runtime_error(error);
                checkCancel(); finished.stage = "encode";
                write(video.encode(*pixels, frame), videoStream);
                const int64_t end = std::min(totalSamples, plan.settings.audioSamplePosition(frame + 1));
                if (end > nextSample) {
                    const auto pcm = executor.audioSamples(nextSample, end, context);
                    if (!pcm) { checkCancel(); throw std::runtime_error(pcm.error); }
                    write(audio.encode(*pcm.value), audioStream);
                    nextSample = end;
                }
                publish({ExportState::Rendering, frame + 1, plan.frameCount, nextSample, totalSamples,
                    0.98 * static_cast<double>(frame + 1) / plan.frameCount});
            }
            checkCancel(); finished.stage = "finalize";
            publish({ExportState::Finalizing, plan.frameCount, plan.frameCount, nextSample, totalSamples, 0.99});
            write(video.drain(), videoStream);
            write(audio.drain(), audioStream);
            muxer.setStreamDuration(videoStream, plan.frameCount, video.timeBase());
            muxer.setStreamDuration(audioStream, totalSamples, audio.timeBase());
            muxer.finish();
            renderer.shutdown();
            checkCancel(); finished.stage = "commit";
            staged.commit();
            finished.state = ExportState::Succeeded;
            finished.stage = "complete";
        } catch (const Cancelled&) {
            finished.state = ExportState::Cancelled;
        } catch (const std::exception& error) {
            finished.state = cancelled.load(std::memory_order_acquire) ? ExportState::Cancelled : ExportState::Failed;
            finished.error = error.what();
        } catch (...) {
            finished.state = ExportState::Failed;
            finished.error = "Unknown export failure";
        }
        complete(std::move(finished));
    }
};

ExportEngine::ExportEngine() : impl_(std::make_unique<Impl>(*this)) {}
ExportEngine::~ExportEngine() { shutdown(); }
bool ExportEngine::start(ExportRequest request, std::string* error) {
    if (currentExportWorker == this) {
        if (error) *error = "Start a new export outside worker callbacks";
        return false;
    }
    std::lock_guard lifecycle(impl_->lifecycle);
    if (impl_->active.load(std::memory_order_acquire)) {
        if (error) *error = "An export is already running";
        return false;
    }
    if (request.outputPath.empty()) { if (error) *error = "Export output path is empty"; return false; }
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->cancelled.store(false, std::memory_order_release);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->result = {};
        impl_->progress = {};
        impl_->progress.state = ExportState::Preparing;
    }
    impl_->active.store(true, std::memory_order_release);
    try { impl_->worker = std::thread([this, request = std::move(request)]() mutable { impl_->run(std::move(request)); }); }
    catch (const std::exception& failure) {
        impl_->active.store(false, std::memory_order_release);
        if (error) *error = failure.what();
        return false;
    }
    return true;
}
void ExportEngine::cancel() { impl_->cancelled.store(true, std::memory_order_release); }
void ExportEngine::wait() {
    if (currentExportWorker == this) return;
    std::lock_guard lifecycle(impl_->lifecycle);
    if (impl_->worker.joinable() && impl_->worker.get_id() != std::this_thread::get_id()) impl_->worker.join();
}
void ExportEngine::shutdown() { cancel(); wait(); }
bool ExportEngine::running() const { return impl_->active.load(std::memory_order_acquire); }
ExportProgress ExportEngine::progress() const { return impl_->snapshotProgress(); }
ExportResult ExportEngine::result() const { std::lock_guard lock(impl_->mutex); return impl_->result; }

} // namespace heisenberg
