#pragma once

#include <Project/VideoProject.hpp>
#include <MultiMedia/Video/Renderer/FilterGraph/Vulkan/Graph/VulkanGraphDocument.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heisenberg {

enum class ExportVideoCodec { H264, H265 };
enum class ExportState { Idle, Preparing, Rendering, Finalizing, Succeeded, Cancelled, Failed };

struct ExportRequest {
    ProjectSnapshot snapshot;
    std::string outputPath;
    ExportVideoCodec videoCodec = ExportVideoCodec::H264;
    int crf = 20;
    std::string preset = "medium";
    bool overwrite = false;
    bool hardwareDecode = false;
    std::optional<filtergraph::VulkanGraphDocument> filterGraph;
};

struct ExportProgress {
    ExportState state = ExportState::Idle;
    int64_t completedFrames = 0;
    int64_t totalFrames = 0;
    int64_t completedSamples = 0;
    int64_t totalSamples = 0;
    double fraction = 0.0;
};

struct ExportResult {
    ExportState state = ExportState::Idle;
    std::string outputPath;
    std::string stage;
    std::string error;
    std::vector<std::string> warnings;
    bool succeeded() const {
        return state == ExportState::Succeeded;
    }
};

class ExportEngine {
public:
    ExportEngine();
    ~ExportEngine();
    ExportEngine(const ExportEngine&) = delete;
    ExportEngine& operator=(const ExportEngine&) = delete;

    bool start(ExportRequest request, std::string* error = nullptr);
    void cancel();
    void wait();
    void shutdown();
    bool running() const;
    ExportProgress progress() const;
    ExportResult result() const;

    std::function<void(const ExportProgress&)> onProgress;
    std::function<void(const ExportResult&)> onFinished;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heisenberg
