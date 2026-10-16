#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace cal {

enum class Repeat { None, Daily, Weekly, Monthly };

std::string to_string(Repeat r);
std::optional<Repeat> repeat_from_string(const std::string& text);

struct Event {
    long id = 0;
    std::string title;
    std::string message;
    std::time_t due = 0;
    Repeat repeat = Repeat::None;
};

// "%Y-%m-%d %H:%M:%S" by default, in local time.
std::string format_time(std::time_t t, const char* fmt = "%Y-%m-%d %H:%M:%S");

// "in 2h 5m", "in 12s", "overdue" ...
std::string format_relative(std::time_t due, std::time_t now);

// Parses a user supplied time expression relative to `now` (local time).
//
//   HH:MM            today at HH:MM, or tomorrow when that already passed
//   HH:MM:SS         same, with seconds
//   today HH:MM      today, even if the time already passed
//   tomorrow HH:MM   tomorrow (09:00 when the time is omitted)
//   mon 17:00        the next such weekday (09:00 when the time is omitted)
//   YYYY-MM-DD[ HH:MM[:SS]] / YYYY-MM-DDTHH:MM[:SS]
//                    a date; 09:00 when the time is omitted
//   +90s +15m +2h30m +1d +1w
//                    a duration from now (units: s, m, h, d, w)
//   @1700000000      a raw unix timestamp
std::optional<std::time_t> parse_when(const std::string& expr, std::time_t now,
                                      std::string& error);

class Calendar {
public:
    explicit Calendar(std::string path);

    const std::string& path() const { return path_; }

    // A missing file is not an error; it simply yields an empty calendly.
    bool load(std::string& error);
    bool save(std::string& error) const;

    // Reloads when the file changed on disk (someone ran `calendly add`).
    bool reload_if_changed(bool& changed, std::string& error);

    Event& add(const std::string& title, std::time_t due, Repeat repeat,
               const std::string& message);
    bool remove(long id);
    void clear();

    const std::vector<Event>& events() const { return events_; }
    const Event* find(long id) const;

    // Earliest due time, or 0 when the calendly is empty.
    std::time_t next_due() const;

    // Returns every event that is due at or before `now`. Repeating events are
    // rescheduled to their next occurrence, one-shot events are erased.
    // The caller is responsible for calling save().
    std::vector<Event> take_due(std::time_t now);

    static std::string default_path();

private:
    void refresh_stamp() const;

    std::string path_;
    std::vector<Event> events_;
    long next_id_ = 1;
    mutable std::time_t mtime_ = 0;
    mutable long long size_ = -1;
};

} // namespace cal
