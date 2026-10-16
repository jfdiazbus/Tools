#include "notifier.hpp"

#include "paths.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cal {

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

bool env_is_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}

// Resolves `name` against $PATH. Absolute or relative paths are checked
// directly and returned as is.
std::string find_in_path(const std::string& name) {
    if (name.empty()) return "";

    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0 ? name : std::string();
    }

    std::string path = env_or_empty("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        std::string dir = path.substr(start, end == std::string::npos
                                                ? std::string::npos
                                                : end - start);
        if (dir.empty()) dir = ".";
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

// Runs `args` in a detached grandchild so no zombie or blocked daemon is left
// behind, and so the notification program cannot stall the caller.
bool spawn_detached(const std::vector<std::string>& args) {
    if (args.empty() || args[0].empty()) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(1);
        if (pid2 > 0) _exit(0);

        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 1024) max_fd = 1024;
        for (int fd = 3; fd < max_fd; ++fd) close(fd);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execv(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

std::string default_notify_command() {
    std::string name = env_or_empty("CALENDLY_NOTIFY_CMD");
    if (name.empty()) name = "notify-send";
    return find_in_path(name);
}

bool desktop_available(const std::string& command) {
    if (env_is_set("CALENDLY_FORCE_STDOUT")) return false;

    if (!env_is_set("CALENDLY_FORCE_DESKTOP")) {
        if (!env_is_set("DISPLAY") && !env_is_set("WAYLAND_DISPLAY")) return false;
    }

    std::string cmd = command.empty() ? default_notify_command() : find_in_path(command);
    return !cmd.empty();
}

bool notify(const std::string& title, const std::string& body,
            const NotifyOptions& opts) {
    if (!opts.desktop) return false;

    std::string cmd = opts.command.empty() ? default_notify_command()
                                           : find_in_path(opts.command);
    if (cmd.empty()) return false;
    if (!env_is_set("CALENDLY_FORCE_DESKTOP") && !env_is_set("DISPLAY") &&
        !env_is_set("WAYLAND_DISPLAY")) {
        return false;
    }

    std::vector<std::string> args;
    args.push_back(cmd);
    args.push_back("--app-name=calendly");
    args.push_back("--urgency=normal");
    args.push_back("--icon=appointment-soon");
    args.push_back(title);
    if (!body.empty()) args.push_back(body);

    return spawn_detached(args);
}

std::string log_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

void log_line(const std::string& path, const std::string& text) {
    if (path.empty()) return;

    std::string error;
    if (!ensure_parent_dir(path, error)) return;

    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out << "[" << log_timestamp() << "] " << text << "\n";
}

} // namespace cal
