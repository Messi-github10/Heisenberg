#include "VideoProject.hpp"

namespace heisenberg {

VideoProject::VideoProject() : ownerId_(makeUuid()) {}
ProjectSnapshot VideoProject::snapshot() const {
    ProjectSnapshot result;
    result.project_ = editor_.project();
    result.manifest_ = editor_.manifest();
    result.root_ = editor_.projectDir();
    result.revision_ = revision();
    result.documentRevision_ = editor_.documentRevision();
    result.transient_ = editor_.hasClipEffectsTransaction();
    result.generation_ = generation_;
    result.ownerId_ = ownerId_;
    return result;
}
bool VideoProject::loadPackage(const std::string& path, std::string* error) {
    if (editor_.hasClipEffectsTransaction()) {
        if (error) *error = "clip effects transaction is pending";
        return false;
    }
    ProjectFile project;
    MediaManifest manifest;
    std::string root;
    if (!coordinator_.loadPackage(path, project, manifest, root, error)) return false;
    if (!editor_.replaceDocument(std::move(project), std::move(manifest), std::move(root), error)) return false;
    ++generation_;
    savedRevision_ = revision();
    savedDocumentRevision_ = editor_.documentRevision();
    return true;
}
bool VideoProject::savePackage(const std::string& path, std::string* error) {
    return saveSnapshot(snapshot(), path, error);
}
bool VideoProject::save(std::string* error) {
    return savePackage(editor_.projectDir(), error);
}
bool VideoProject::saveAs(const std::string& path, std::string* error) {
    return savePackage(path, error);
}
bool VideoProject::saveSnapshot(const ProjectSnapshot& capture, const std::string& path, std::string* error) {
    if (editor_.hasClipEffectsTransaction()) {
        if (error) *error = "clip effects transaction is pending";
        return false;
    }
    if (capture.transient_) {
        if (error) *error = "snapshot contains a transient clip effects transaction";
        return false;
    }
    if (capture.ownerId_ != ownerId_ || capture.generation_ != generation_) {
        if (error) *error = "snapshot belongs to a different project or document generation";
        return false;
    }
    std::string root;
    if (!coordinator_.savePackage(capture, path, root, error)) return false;
    editor_.setProjectDir(std::move(root));
    // Checkpoint exactly what was written, never the newer live document.
    savedRevision_ = capture.revision();
    savedDocumentRevision_ = capture.documentRevision_;
    return true;
}

} // namespace heisenberg
