#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

struct ExecResult {
    int status {127};
    std::string out;
};

ExecResult run_cmd(const std::string& cmd) {
    ExecResult result;
    std::array<char, 4096> buffer {};
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) {
        return result;
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.out += buffer.data();
    }
    const int rc = pclose(pipe);
    if (WIFEXITED(rc)) {
        result.status = WEXITSTATUS(rc);
    } else {
        result.status = 128;
    }
    return result;
}

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

int main() {
    int failures = 0;

    {
        ExecResult r = run_cmd("./libsh --help");
        if (r.status != 0 || !contains(r.out, "Usage:") || !contains(r.out, "--command")) {
            std::cerr << "FAIL: help\n";
            ++failures;
        }
    }

    {
        ExecResult r = run_cmd("./libsh --version");
        if (r.status != 0 || !contains(r.out, "libsh")) {
            std::cerr << "FAIL: version\n";
            ++failures;
        }
    }

    {
        ExecResult r = run_cmd("./libsh --bad-flag");
        if (r.status != 2 || !contains(r.out, "unknown option")) {
            std::cerr << "FAIL: unknown flag\n";
            ++failures;
        }
    }

    {
        ExecResult r = run_cmd("./libsh --command");
        if (r.status != 2 || !contains(r.out, "missing argument")) {
            std::cerr << "FAIL: missing -c arg\n";
            ++failures;
        }
    }

    {
        ExecResult r = run_cmd("./libsh --command 'echo cli_hardened'");
        if (r.status != 0 || !contains(r.out, "cli_hardened")) {
            std::cerr << "FAIL: command mode\n";
            ++failures;
        }
    }

    if (failures != 0) {
        std::cerr << "test_cli failures=" << failures << '\n';
        return 1;
    }
    std::cout << "test_cli ok\n";
    return 0;
}
