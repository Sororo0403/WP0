#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

class ScriptBuildService {
public:
    static bool BuildIfNeeded(const std::filesystem::path& projectRoot,
                              std::string& error);
    static bool Build(const std::filesystem::path& projectRoot, std::string& error,
                      std::string* output = nullptr);
    static bool GetSourceFingerprint(const std::filesystem::path& projectRoot,
                                     uint64_t& fingerprint, std::string& error);
    static bool ParseDiagnosticLocation(std::string_view outputLine,
                                        std::filesystem::path& sourcePath,
                                        uint32_t& line, uint32_t& column);
};
