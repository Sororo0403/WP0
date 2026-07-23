#pragma once

#include <string>

struct ProjectDescriptor;

class PlayerProjectValidator {
public:
    static bool Validate(const ProjectDescriptor& project, std::string& error);
};
