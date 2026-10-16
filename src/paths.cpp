#include "paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <signal.h>
#include <unistd.h>

namespace cal {

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

std::string home_dir() {
    return env_or_empty("HOME");
}

std::string xdg_dir(const char* var, const char* fallback) {
    std::string v = env_or_empty(var);
    if (!v.empty()) return v;
    std::string home = home_dir();
    if (home.empty()) return ".";
    return home + fallback;
}

// Moves `from` to `to`, unless the destination already exists.
void migrate_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    if (from.empty() || to.empty()) return;
    if (!std::filesystem::exists(from, ec)) return;
    if (std::filesystem::exists(to, ec)) return;

    std::string error;
    if (!ensure_parent_dir(to, error)) return;

    std::filesystem::rename(from, to, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(from, to,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (!ec) std::filesystem::remove(from, ec);
    }
}

// Removes a pid file left behind by the old binary, but only when the process
// it points at is gone.
void discard_stale_pidfile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;

    long pid = 0;
    if (!(in >> pid) || pid <= 0) return;
    in.close();

    if (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM) return;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

std::string expand_user(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    std::string home = home_dir();
    if (home.empty()) return path;
    if (path.size() == 1) return home;
    if (path[1] == '/') return home + path.substr(1);
    return path; // "~user" form is not supported
}

std::string parent_dir(const std::string& path) {
    if (path.empty()) return "";
    return std::filesystem::path(path).parent_path().string();
}

bool ensure_parent_dir(const std::string& path, std::string& error) {
    std::string dir = parent_dir(path);
    if (dir.empty()) return true;

    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) return true;

    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create directory " + dir + ": " + ec.message();
        return false;
    }
    return true;
}

std::string data_dir() {
    return xdg_dir("XDG_DATA_HOME", "/.local/share");
}

std::string state_dir() {
    return xdg_dir("XDG_STATE_HOME", "/.local/state");
}

std::string default_db_path() {
    std::string override = env_or_empty("CALENDLY_DB");
    if (!override.empty()) return expand_user(override);
    return data_dir() + "/calendly/events.db";
}

std::string default_log_path() {
    std::string override = env_or_empty("CALENDLY_LOG");
    if (!override.empty()) return expand_user(override);
    return state_dir() + "/calendly/calendly.log";
}

std::string default_pid_path() {
    std::string override = env_or_empty("CALENDLY_PIDFILE");
    if (!override.empty()) return expand_user(override);
    return state_dir() + "/calendly/calendly.pid";
}

void migrate_legacy_paths() {
    // The tool used to be called "calendar"; move whatever is left behind to
    // the new locations so existing events survive the rename.
    migrate_file(data_dir() + "/calendar/events.db",
                 data_dir() + "/calendly/events.db");
    migrate_file(state_dir() + "/calendar/calendar.log",
                 state_dir() + "/calendly/calendly.log");
    discard_stale_pidfile(state_dir() + "/calendar/calendar.pid");

    std::error_code ignored;
    std::filesystem::remove(state_dir() + "/calendar", ignored);
    std::filesystem::remove(data_dir() + "/calendar", ignored);
}

} // namespace cal
