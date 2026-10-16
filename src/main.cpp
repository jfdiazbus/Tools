#include "calendly.hpp"
#include "notifier.hpp"
#include "paths.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_stop = 0;

void handle_signal(int) {
    g_stop = 1;
}

const char* kVersion = "0.1.0";

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

bool parse_long_long(const std::string& text, long long& out) {
    std::string s = trim(text);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

bool parse_seconds(const std::string& text, double& out) {
    std::string s = trim(text);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    double value = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

void sleep_ms(long long milliseconds) {
    if (milliseconds <= 0) return;
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(milliseconds / 1000);
    ts.tv_nsec = static_cast<long>((milliseconds % 1000) * 1000000L);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !g_stop) {
    }
}

bool process_alive(long pid) {
    if (pid <= 0) return false;
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
}

long read_pidfile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return 0;
    long long pid = 0;
    std::string token;
    if (!(in >> token)) return 0;
    if (!parse_long_long(token, pid)) return 0;
    return pid > 0 ? static_cast<long>(pid) : 0;
}

bool write_pidfile(const std::string& path, std::string& error) {
    if (!cal::ensure_parent_dir(path, error)) return false;
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "cannot write " + path + ": " + std::strerror(errno);
        return false;
    }
    out << static_cast<long>(getpid()) << "\n";
    out.flush();
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

void remove_own_pidfile(const std::string& path) {
    if (read_pidfile(path) == static_cast<long>(getpid())) std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// argument parsing
// ---------------------------------------------------------------------------

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    bool flag(const std::string& name) const {
        auto it = options.find(name);
        return it != options.end() && it->second != "0";
    }

    bool has(const std::string& name) const { return options.count(name) != 0; }

    std::string opt(const std::string& name, const std::string& fallback = "") const {
        auto it = options.find(name);
        return it == options.end() ? fallback : it->second;
    }
};

Args parse_args(const std::vector<std::string>& tokens,
                const std::set<std::string>& bool_flags, std::string& error) {
    Args args;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];

        if (token == "--") {
            for (++i; i < tokens.size(); ++i) args.positional.push_back(tokens[i]);
            break;
        }

        if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
            size_t eq = token.find('=');
            std::string name;
            if (eq != std::string::npos) {
                name = token.substr(2, eq - 2);
                if (name.empty()) {
                    error = "invalid option '" + token + "'";
                    return Args{};
                }
                args.options[name] = token.substr(eq + 1);
                continue;
            }

            name = token.substr(2);
            if (bool_flags.count(name)) {
                args.options[name] = "1";
                continue;
            }
            if (i + 1 >= tokens.size()) {
                error = "option '--" + name + "' requires a value";
                return Args{};
            }
            args.options[name] = tokens[++i];
            continue;
        }

        if (token.size() >= 2 && token[0] == '-' && token != "-") {
            std::string name = token.substr(1);
            if (bool_flags.count(name)) {
                args.options[name] = "1";
                continue;
            }
            if (i + 1 >= tokens.size()) {
                error = "option '-" + name + "' requires a value";
                return Args{};
            }
            args.options[name] = tokens[++i];
            continue;
        }

        args.positional.push_back(token);
    }

    // short aliases
    static const std::pair<const char*, const char*> aliases[] = {
        {"a", "at"},     {"m", "message"}, {"r", "repeat"}, {"i", "in"},
        {"d", "db"},     {"l", "log"},     {"p", "pidfile"}, {"n", "interval"},
    };
    for (const auto& alias : aliases) {
        auto it = args.options.find(alias.first);
        if (it != args.options.end() && !args.options.count(alias.second)) {
            args.options[alias.second] = it->second;
        }
    }

    return args;
}

// ---------------------------------------------------------------------------
// runtime configuration
// ---------------------------------------------------------------------------

struct Runtime {
    std::string db;
    std::string log;
    std::string pidfile;
    long long interval_ms = 1000;
    bool force_stdout = false;
    bool once = false;
};

bool build_runtime(const Args& args, Runtime& runtime, std::string& error) {
    std::string db = args.opt("db");
    runtime.db = db.empty() ? cal::Calendar::default_path()
                            : cal::expand_user(db);

    std::string log = args.opt("log");
    runtime.log = log.empty() ? cal::default_log_path() : cal::expand_user(log);

    std::string pidfile = args.opt("pidfile");
    runtime.pidfile =
        pidfile.empty() ? cal::default_pid_path() : cal::expand_user(pidfile);

    std::string interval = args.opt("interval");
    if (!interval.empty()) {
        double seconds = 0.0;
        if (!parse_seconds(interval, seconds) || seconds <= 0.0) {
            error = "invalid --interval '" + interval + "'";
            return false;
        }
        if (seconds < 0.05) seconds = 0.05;
        runtime.interval_ms = static_cast<long long>(seconds * 1000.0);
    }

    runtime.force_stdout = args.flag("stdout");
    runtime.once = args.flag("once");
    return true;
}

bool load_calendar(cal::Calendar& calendar, std::string& error) {
    if (!calendar.load(error)) {
        std::cerr << "calendly: " << error << "\n";
        return false;
    }
    return true;
}

bool save_calendar(const cal::Calendar& calendar) {
    std::string error;
    if (!calendar.save(error)) {
        std::cerr << "calendly: " << error << "\n";
        return false;
    }
    return true;
}

std::vector<const cal::Event*> ordered_events(const cal::Calendar& calendar) {
    std::vector<const cal::Event*> ordered;
    ordered.reserve(calendar.events().size());
    for (const cal::Event& e : calendar.events()) ordered.push_back(&e);
    std::sort(ordered.begin(), ordered.end(),
              [](const cal::Event* a, const cal::Event* b) {
                  if (a->due != b->due) return a->due < b->due;
                  return a->id < b->id;
              });
    return ordered;
}

std::string event_label(const cal::Event& e) {
    std::string label = e.title;
    if (!e.message.empty()) label += " (" + e.message + ")";
    return label;
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

int cmd_add(const Args& args, const Runtime& runtime) {
    if (args.positional.empty()) {
        std::cerr << "calendly add: missing the event title\n";
        return 2;
    }

    std::string title;
    for (size_t i = 0; i < args.positional.size(); ++i) {
        if (i) title += ' ';
        title += args.positional[i];
    }
    title = trim(title);
    if (title.empty()) {
        std::cerr << "calendly add: the event title is empty\n";
        return 2;
    }

    std::string when = args.opt("at");
    if (when.empty()) {
        std::string relative = args.opt("in");
        if (!relative.empty()) {
            if (relative[0] != '+') relative.insert(relative.begin(), '+');
            when = relative;
        }
    }
    if (when.empty()) {
        std::cerr << "calendly add: --at <when> is required (e.g. --at 18:30, "
                     "--at tomorrow 09:00, --in 45m)\n";
        return 2;
    }

    std::string when_error;
    auto due = cal::parse_when(when, std::time(nullptr), when_error);
    if (!due) {
        std::cerr << "calendly add: " << when_error << "\n";
        return 2;
    }

    auto repeat = cal::repeat_from_string(args.opt("repeat", "none"));
    if (!repeat) {
        std::cerr << "calendly add: invalid --repeat '" << args.opt("repeat")
                  << "' (use none, daily, weekly or monthly)\n";
        return 2;
    }

    cal::Calendar calendar(runtime.db);
    std::string error;
    if (!load_calendar(calendar, error)) return 1;

    cal::Event& event = calendar.add(title, *due, *repeat, args.opt("message"));
    if (!save_calendar(calendar)) return 1;

    std::printf("added #%ld  %s  (%s)  [%s]  %s\n", event.id,
                cal::format_time(event.due).c_str(),
                cal::format_relative(event.due, std::time(nullptr)).c_str(),
                cal::to_string(event.repeat).c_str(), event.title.c_str());
    return 0;
}

int cmd_list(const Args& args, const Runtime& runtime) {
    cal::Calendar calendar(runtime.db);
    std::string error;
    if (!load_calendar(calendar, error)) return 1;

    const std::vector<const cal::Event*> ordered = ordered_events(calendar);
    const std::time_t now = std::time(nullptr);

    if (args.flag("json")) {
        std::cout << "[";
        for (size_t i = 0; i < ordered.size(); ++i) {
            const cal::Event& e = *ordered[i];
            if (i) std::cout << ",";
            std::cout << "\n  {\"id\":" << e.id
                      << ",\"title\":\"" << json_escape(e.title) << "\""
                      << ",\"message\":\"" << json_escape(e.message) << "\""
                      << ",\"due\":" << static_cast<long long>(e.due)
                      << ",\"due_local\":\"" << cal::format_time(e.due) << "\""
                      << ",\"repeat\":\"" << cal::to_string(e.repeat) << "\""
                      << ",\"in_seconds\":"
                      << static_cast<long long>(e.due) - static_cast<long long>(now)
                      << "}";
        }
        std::cout << (ordered.empty() ? "]\n" : "\n]\n");
        return 0;
    }

    if (ordered.empty()) {
        std::printf("no events scheduled\n");
        return 0;
    }

    std::printf("%-4s  %-19s  %-12s  %-8s  %s\n", "ID", "WHEN", "IN", "REPEAT",
                "TITLE");
    for (const cal::Event* e : ordered) {
        std::printf("%-4ld  %-19s  %-12s  %-8s  %s\n", e->id,
                    cal::format_time(e->due).c_str(),
                    cal::format_relative(e->due, now).c_str(),
                    cal::to_string(e->repeat).c_str(), event_label(*e).c_str());
    }
    return 0;
}

int cmd_remove(const Args& args, const Runtime& runtime) {
    if (args.positional.empty()) {
        std::cerr << "calendly remove: expected at least one event id\n";
        return 2;
    }

    std::vector<long> ids;
    for (const std::string& token : args.positional) {
        long long id = 0;
        if (!parse_long_long(token, id) || id <= 0) {
            std::cerr << "calendly remove: invalid id '" << token << "'\n";
            return 2;
        }
        ids.push_back(static_cast<long>(id));
    }

    cal::Calendar calendar(runtime.db);
    std::string error;
    if (!load_calendar(calendar, error)) return 1;

    int removed = 0;
    for (long id : ids) {
        if (calendar.remove(id)) {
            ++removed;
            std::printf("removed #%ld\n", id);
        } else {
            std::printf("no event with id %ld\n", id);
        }
    }

    if (removed > 0 && !save_calendar(calendar)) return 1;
    return removed > 0 ? 0 : 1;
}

int cmd_clear(const Args& args, const Runtime& runtime) {
    cal::Calendar calendar(runtime.db);
    std::string error;
    if (!load_calendar(calendar, error)) return 1;

    const size_t count = calendar.events().size();
    if (count == 0) {
        std::printf("nothing to clear\n");
        return 0;
    }

    if (!args.flag("yes") && !args.flag("force")) {
        std::cerr << "calendly clear: this deletes " << count
                  << " event(s); re-run with --yes to confirm\n";
        return 2;
    }

    calendar.clear();
    if (!save_calendar(calendar)) return 1;
    std::printf("cleared %zu event(s)\n", count);
    return 0;
}

int cmd_notify_test(const Runtime& runtime) {
    cal::NotifyOptions options;
    options.desktop = !runtime.force_stdout && cal::desktop_available();

    const std::string title = "calendly";
    const std::string body = "notifications are working";

    if (cal::notify(title, body, options)) {
        std::printf("sent a desktop notification via %s\n",
                    (options.command.empty() ? cal::default_notify_command()
                                             : options.command)
                        .c_str());
        cal::log_line(runtime.log, "notify-test sent to desktop");
        return 0;
    }

    std::printf("\a%s: %s\n", title.c_str(), body.c_str());
    std::printf("desktop notifications unavailable, printed on stdout instead\n");
    cal::log_line(runtime.log, "notify-test printed on stdout");
    return 0;
}

int run_daemon(const Runtime& runtime) {
    cal::Calendar calendar(runtime.db);
    std::string error;

    if (!calendar.load(error)) {
        cal::log_line(runtime.log, "error: " + error);
        std::cerr << "calendly: " << error << "\n";
        return 1;
    }

    cal::NotifyOptions options;
    options.desktop = !runtime.force_stdout && cal::desktop_available();

    if (!write_pidfile(runtime.pidfile, error)) {
        cal::log_line(runtime.log, "warning: " + error);
    }

    cal::log_line(runtime.log, "daemon started pid=" + std::to_string(getpid()) +
                                   " db=" + runtime.db +
                                   " notify=" +
                                   (options.desktop ? "desktop" : "stdout"));
    if (!options.desktop) {
        cal::log_line(runtime.log,
                      "desktop notifications unavailable; alerts go to stdout/log");
    }

    while (!g_stop) {
        const std::time_t now = std::time(nullptr);
        std::vector<cal::Event> fired = calendar.take_due(now);

        if (!fired.empty()) {
            for (const cal::Event& event : fired) {
                std::string record = "fired #" + std::to_string(event.id) + " " +
                                     cal::format_time(event.due) + " " + event.title;
                if (!event.message.empty()) record += " -- " + event.message;

                if (!cal::notify(event.title, event.message, options)) {
                    std::string text = event.title;
                    if (!event.message.empty()) text += " -- " + event.message;
                    std::printf("\a%s\n", text.c_str());
                    std::fflush(stdout);
                    record += " [stdout]";
                }
                cal::log_line(runtime.log, record);

                if (event.repeat != cal::Repeat::None) {
                    const cal::Event* current = calendar.find(event.id);
                    if (current != nullptr) {
                        cal::log_line(runtime.log,
                                      "rescheduled #" + std::to_string(event.id) +
                                          " to " + cal::format_time(current->due));
                    }
                }
            }

            std::string save_error;
            if (!calendar.save(save_error)) cal::log_line(runtime.log, "error: " + save_error);
        }

        bool changed = false;
        std::string reload_error;
        if (!calendar.reload_if_changed(changed, reload_error)) {
            cal::log_line(runtime.log, "error: " + reload_error);
        } else if (changed) {
            cal::log_line(runtime.log, "reloaded " +
                                           std::to_string(calendar.events().size()) +
                                           " event(s)");
        }

        if (runtime.once) break;

        long long wait_ms = runtime.interval_ms;
        const std::time_t next = calendar.next_due();
        if (next > 0) {
            long long until = (static_cast<long long>(next) -
                               static_cast<long long>(std::time(nullptr))) *
                              1000;
            if (until < wait_ms) wait_ms = until;
        }
        if (wait_ms < 50) wait_ms = 50;

        sleep_ms(wait_ms);
    }

    cal::log_line(runtime.log, "daemon stopped pid=" + std::to_string(getpid()));
    remove_own_pidfile(runtime.pidfile);
    return 0;
}

int cmd_daemon(const Args& args, const Runtime& runtime) {
    const long existing = read_pidfile(runtime.pidfile);
    if (process_alive(existing) && existing != static_cast<long>(getpid()) &&
        !args.flag("force")) {
        std::cerr << "calendly daemon: already running (pid " << existing
                  << "); use --force to start another one\n";
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    return run_daemon(runtime);
}

int cmd_start(const Args& args, const Runtime& runtime) {
    const long existing = read_pidfile(runtime.pidfile);
    if (process_alive(existing) && !args.flag("force")) {
        std::cerr << "calendly start: already running (pid " << existing << ")\n";
        return 1;
    }

    std::string error;
    if (!cal::ensure_parent_dir(runtime.log, error)) {
        std::cerr << "calendly start: " << error << "\n";
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "calendly start: fork failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    if (pid > 0) {
        // Wait for the daemon to publish its pidfile so that `stop` works
        // immediately after `start` returns.
        for (int i = 0; i < 100; ++i) {
            if (read_pidfile(runtime.pidfile) == static_cast<long>(pid)) break;
            sleep_ms(20);
        }
        std::printf("daemon started (pid %ld)\n", static_cast<long>(pid));
        std::printf("  db:  %s\n", runtime.db.c_str());
        std::printf("  log: %s\n", runtime.log.c_str());
        return 0;
    }

    // child: detach and become the daemon
    if (setsid() < 0) {
        std::cerr << "calendly start: setsid failed: " << std::strerror(errno) << "\n";
        _exit(1);
    }

    int log_fd = open(runtime.log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        if (log_fd > 2) close(log_fd);
    }
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        if (devnull > 2) close(devnull);
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    const int status = run_daemon(runtime);
    _exit(status);
}

int cmd_stop(const Args&, const Runtime& runtime) {
    const long pid = read_pidfile(runtime.pidfile);
    if (pid <= 0) {
        std::printf("daemon is not running\n");
        return 1;
    }

    if (!process_alive(pid)) {
        std::remove(runtime.pidfile.c_str());
        std::printf("daemon is not running (removed the stale pidfile)\n");
        return 1;
    }

    if (kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        std::cerr << "calendly stop: cannot signal pid " << pid << ": "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    for (int i = 0; i < 50 && process_alive(pid); ++i) sleep_ms(100);
    if (process_alive(pid)) {
        kill(static_cast<pid_t>(pid), SIGKILL);
        std::printf("daemon (pid %ld) did not stop, killed it\n", pid);
    } else {
        std::printf("daemon stopped (pid %ld)\n", pid);
    }

    std::remove(runtime.pidfile.c_str());
    return 0;
}

int cmd_status(const Args& args, const Runtime& runtime) {
    cal::Calendar calendar(runtime.db);
    std::string error;
    if (!load_calendar(calendar, error)) return 1;

    const long pid = read_pidfile(runtime.pidfile);
    const bool running = process_alive(pid);
    const std::vector<const cal::Event*> ordered = ordered_events(calendar);
    const std::time_t now = std::time(nullptr);

    if (args.flag("json")) {
        std::cout << "{\"daemon_running\":" << (running ? "true" : "false")
                  << ",\"daemon_pid\":" << (running ? pid : 0) << ",\"db\":\""
                  << json_escape(runtime.db) << "\",\"event_count\":"
                  << calendar.events().size() << "}\n";
        return 0;
    }

    if (running) {
        std::printf("daemon:  running (pid %ld)\n", pid);
    } else if (pid > 0) {
        std::printf("daemon:  not running (stale pidfile, pid %ld)\n", pid);
    } else {
        std::printf("daemon:  not running\n");
    }
    std::printf("db:      %s (%zu event(s))\n", runtime.db.c_str(),
                calendar.events().size());
    std::printf("log:     %s\n", runtime.log.c_str());
    std::printf("pidfile: %s\n", runtime.pidfile.c_str());

    if (ordered.empty()) {
        std::printf("next:    nothing scheduled\n");
        return 0;
    }

    std::printf("next:\n");
    const size_t limit = std::min<size_t>(ordered.size(), 5);
    for (size_t i = 0; i < limit; ++i) {
        const cal::Event& e = *ordered[i];
        std::printf("  #%-4ld %s  (%s)  [%s]  %s\n", e.id,
                    cal::format_time(e.due).c_str(),
                    cal::format_relative(e.due, now).c_str(),
                    cal::to_string(e.repeat).c_str(), event_label(e).c_str());
    }
    if (ordered.size() > limit) {
        std::printf("  ... and %zu more\n", ordered.size() - limit);
    }
    return 0;
}

void print_help() {
    std::printf(
        "calendly %s -- schedule events from the terminal and get notified on time\n"
        "\n"
        "USAGE\n"
        "  calendly <command> [options]\n"
        "\n"
        "COMMANDS\n"
        "  add <title> --at <when>   schedule a new event\n"
        "  list                      list scheduled events (--json)\n"
        "  remove <id>...            delete events by id\n"
        "  clear --yes               delete every event\n"
        "  start                     start the notification daemon in the background\n"
        "  stop                      stop the notification daemon\n"
        "  daemon                    run the notification daemon in the foreground\n"
        "  status                    show the daemon state and the next events\n"
        "  notify-test               send a test notification\n"
        "  help                      show this help\n"
        "\n"
        "WHEN\n"
        "  18:30                    today at 18:30, or tomorrow if that already passed\n"
        "  18:30:45                 same, with seconds\n"
        "  today 18:30              today, even when the time already passed\n"
        "  tomorrow 09:00           tomorrow (09:00 when the time is omitted)\n"
        "  mon 17:00                the next such weekday (09:00 when omitted)\n"
        "  2026-10-16 18:30         an absolute date/time (\"T\" also works)\n"
        "  2026-10-16               a date, at 09:00\n"
        "  +45m   +2h30m   +1d      a duration from now (s, m, h, d, w)\n"
        "  @1700000000              a raw unix timestamp\n"
        "\n"
        "ADD OPTIONS\n"
        "  --at <when>              when the event is due (required; quote it when\n"
        "                           it contains a space, e.g. --at \"fri 17:00\")\n"
        "  --in <duration>          shortcut for --at +<duration>\n"
        "  --repeat <n|daily|weekly|monthly>\n"
        "  --message <text>         extra text shown with the notification\n"
        "\n"
        "DAEMON OPTIONS\n"
        "  --interval <seconds>     how often the event file is re-checked\n"
        "                           (default 1)\n"
        "  --once                   fire everything that is due, then exit\n"
        "  --stdout                 print alerts instead of using notify-send\n"
        "  --force                  start even when another daemon is running\n"
        "\n"
        "GLOBAL OPTIONS\n"
        "  --db <path>              event file (default $XDG_DATA_HOME/calendly/events.db)\n"
        "  --log <path>             daemon log file\n"
        "  --pidfile <path>         daemon pid file\n"
        "\n"
        "ENVIRONMENT\n"
        "  CALENDLY_DB              same as --db\n"
        "  CALENDLY_LOG             same as --log\n"
        "  CALENDLY_PIDFILE         same as --pidfile\n"
        "  CALENDLY_NOTIFY_CMD      notification program (default notify-send)\n"
        "  CALENDLY_FORCE_STDOUT=1  never use the desktop, always print\n"
        "  CALENDLY_FORCE_DESKTOP=1 notify even without DISPLAY/WAYLAND_DISPLAY\n"
        "\n"
        "EXAMPLES\n"
        "  calendly add \"stand up\" --at 18:30 --repeat daily\n"
        "  calendly add \"deploy\" --in 45m --message \"freeze is over\"\n"
        "  calendly add \"week review\" --at \"fri 17:00\" --repeat weekly\n"
        "  calendly start && calendly list\n",
        kVersion);
}

} // namespace

int main(int argc, char** argv) {
    cal::migrate_legacy_paths();

    std::vector<std::string> tokens;
    for (int i = 1; i < argc; ++i) tokens.push_back(argv[i]);

    if (tokens.empty()) {
        print_help();
        return 2;
    }

    const std::string command = tokens[0];
    if (command == "help" || command == "--help" || command == "-h") {
        print_help();
        return 0;
    }
    if (command == "version" || command == "--version" || command == "-v") {
        std::printf("calendly %s\n", kVersion);
        return 0;
    }

    std::set<std::string> bool_flags;
    if (command == "list" || command == "status") bool_flags = {"json"};
    else if (command == "clear") bool_flags = {"yes", "force"};
    else if (command == "daemon") bool_flags = {"stdout", "force", "once"};
    else if (command == "start") bool_flags = {"stdout", "force"};
    else if (command == "notify-test") bool_flags = {"stdout"};
    else if (command != "add" && command != "remove" && command != "stop") {
        std::cerr << "calendly: unknown command '" << command
                  << "' (try 'calendly help')\n";
        return 2;
    }

    const std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
    std::string parse_error;
    Args args = parse_args(rest, bool_flags, parse_error);
    if (!parse_error.empty()) {
        std::cerr << "calendly: " << parse_error << "\n";
        return 2;
    }

    Runtime runtime;
    std::string runtime_error;
    if (!build_runtime(args, runtime, runtime_error)) {
        std::cerr << "calendly: " << runtime_error << "\n";
        return 2;
    }

    if (command == "add") return cmd_add(args, runtime);
    if (command == "list") return cmd_list(args, runtime);
    if (command == "remove") return cmd_remove(args, runtime);
    if (command == "clear") return cmd_clear(args, runtime);
    if (command == "daemon") return cmd_daemon(args, runtime);
    if (command == "start") return cmd_start(args, runtime);
    if (command == "stop") return cmd_stop(args, runtime);
    if (command == "status") return cmd_status(args, runtime);
    if (command == "notify-test") return cmd_notify_test(runtime);

    std::cerr << "calendly: unknown command '" << command << "'\n";
    return 2;
}
