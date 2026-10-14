#pragma once

#include "ProjectFile.hpp"

namespace heisenberg {

class ProjectCodec {
public:
    static bool decode(const std::string& text, ProjectFile& result,
                       std::string* error = nullptr);
    static std::string encode(const ProjectFile& project);
};

} // namespace heisenberg
