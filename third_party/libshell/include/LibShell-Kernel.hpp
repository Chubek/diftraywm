#pragma once

#include "LibShell.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lsh::kernel {

struct Version {
    int major {0};
    int minor {1};
    int patch {0};
    std::string label {"draft"};
};

struct Metadata {
    std::string name;
    std::vector<std::string> aliases;
    Version version;
    std::string summary;
    std::string documentation;
};

struct Invocation {
    std::vector<std::string> argv;
    Environment* environment {nullptr};
    Writer* stdin_writer {nullptr};
    Writer* stdout_writer {nullptr};
    Writer* stderr_writer {nullptr};
};

class Kernel {
public:
    virtual ~Kernel() = default;

    [[nodiscard]] virtual const Metadata& metadata() const noexcept = 0;

    virtual Result<void> load() { return {}; }
    virtual Result<void> initialize(Environment&) { return {}; }
    virtual Result<ExitStatus> execute(const Invocation& invocation) = 0;
    virtual Result<void> shutdown() { return {}; }
};

class FunctionKernel final : public Kernel {
public:
    using ExecuteFn = std::function<Result<ExitStatus>(const Invocation&)>;

    FunctionKernel(Metadata metadata, ExecuteFn execute)
        : metadata_(std::move(metadata)), execute_(std::move(execute)) {}

    [[nodiscard]] const Metadata& metadata() const noexcept override { return metadata_; }

    Result<ExitStatus> execute(const Invocation& invocation) override {
        if (!execute_) {
            return Diagnostic {ErrorCode::execution_failed, "function kernel has no execute handler", metadata_.name};
        }
        return execute_(invocation);
    }

private:
    Metadata metadata_;
    ExecuteFn execute_;
};

class Registry {
public:
    Result<void> add(std::shared_ptr<Kernel> kernel) {
        if (!kernel || kernel->metadata().name.empty()) {
            return Diagnostic {ErrorCode::invalid_graph, "kernel registration requires a named kernel", {}};
        }

        const auto& metadata = kernel->metadata();
        kernels_[metadata.name] = kernel;
        for (const std::string& alias : metadata.aliases) {
            aliases_[alias] = metadata.name;
        }
        return {};
    }

    [[nodiscard]] std::shared_ptr<Kernel> find(std::string_view name) const {
        auto direct = kernels_.find(std::string(name));
        if (direct != kernels_.end()) {
            return direct->second;
        }

        auto alias = aliases_.find(std::string(name));
        if (alias == aliases_.end()) {
            return {};
        }

        auto resolved = kernels_.find(alias->second);
        return resolved == kernels_.end() ? std::shared_ptr<Kernel> {} : resolved->second;
    }

    [[nodiscard]] std::vector<Metadata> list() const {
        std::vector<Metadata> metadata;
        metadata.reserve(kernels_.size());
        for (const auto& [_, kernel] : kernels_) {
            metadata.push_back(kernel->metadata());
        }
        return metadata;
    }

private:
    std::map<std::string, std::shared_ptr<Kernel>, std::less<>> kernels_;
    std::map<std::string, std::string, std::less<>> aliases_;
};

struct PackageManifest {
    Metadata metadata;
    std::vector<std::string> entrypoints;
    std::vector<std::string> permissions;
};

class PackageLoader {
public:
    virtual ~PackageLoader() = default;
    virtual Result<PackageManifest> inspect(std::string_view path) = 0;
    virtual Result<std::shared_ptr<Kernel>> load(std::string_view path) = 0;
};

} // namespace lsh::kernel
