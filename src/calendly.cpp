#include "calendly.hpp"

#include "paths.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace cal {

namespace {

constexpr const char* kFormatTag = "# calendly events -- format v1 "
                                   "(id|repeat|due_epoch|title|message)";
constexpr const char* kNextIdTag = "# next_id=";

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool parse_integer(const std::string& s, long long& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

bool is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int mon0) {
    static const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon0 < 0 || mon0 > 11) return 30;
    if (mon0 == 1 && is_leap(year)) return 29;
    return table[mon0];
}

bool valid_date(int year, int mon0, int mday) {
    if (mon0 < 0 || mon0 > 11) return false;
    if (mday < 1 || mday > days_in_month(year, mon0)) return false;
    return true;
}

std::time_t add_days(std::time_t t, int days) {
    std::tm tm{};
    localtime_r(&t, &tm);
    tm.tm_mday += days;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t add_months(std::time_t t, int months) {
    std::tm base{};
    localtime_r(&t, &base);
    const int wanted_day = base.tm_mday;

    // Anchor on day 1 so that mktime cannot overflow into the next month,
    // then clamp the day (Jan 31 + 1 month -> Feb 28/29).
    std::tm anchor = base;
    anchor.tm_mday = 1;
    anchor.tm_mon += months;
    anchor.tm_isdst = -1;
    std::time_t pushed = std::mktime(&anchor);

    std::tm result{};
    localtime_r(&pushed, &result);
    int limit = days_in_month(result.tm_year + 1900, result.tm_mon);
    result.tm_mday = std::min(wanted_day, limit);
    result.tm_isdst = -1;
    return std::mktime(&result);
}

// Local time on the day of `day_ref` at `seconds_of_day`.
std::time_t local_at(std::time_t day_ref, int seconds_of_day) {
    std::tm tm{};
    localtime_r(&day_ref, &tm);
    tm.tm_hour = seconds_of_day / 3600;
    tm.tm_min = (seconds_of_day / 60) % 60;
    tm.tm_sec = seconds_of_day % 60;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::optional<int> parse_time_of_day(const std::string& s) {
    std::vector<int> parts;
    size_t i = 0;
    while (true) {
        size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j == i) return std::nullopt;

        int value = 0;
        for (size_t k = i; k < j; ++k) {
            value = value * 10 + (s[k] - '0');
            if (value > 999) return std::nullopt;
        }
        parts.push_back(value);

        i = j;
        if (i >= s.size()) break;
        if (s[i] != ':') return std::nullopt;
        ++i;
        if (i >= s.size()) return std::nullopt;
    }

    if (parts.size() < 2 || parts.size() > 3) return std::nullopt;
    if (parts[0] > 23 || parts[1] > 59) return std::nullopt;
    if (parts.size() == 3 && parts[2] > 60) return std::nullopt;

    return parts[0] * 3600 + parts[1] * 60 + (parts.size() == 3 ? parts[2] : 0);
}

std::optional<int> parse_weekday(const std::string& s) {
    static const char* names[7][2] = {
        {"sun", "sunday"}, {"mon", "monday"}, {"tue", "tuesday"},
        {"wed", "wednesday"}, {"thu", "thursday"}, {"fri", "friday"},
        {"sat", "saturday"},
    };
    for (int day = 0; day < 7; ++day) {
        for (int alias = 0; alias < 2; ++alias) {
            if (s == names[day][alias]) return day;
        }
    }
    return std::nullopt;
}

// Reads exactly `digits` decimal digits at `i` and advances `i`.
bool read_fixed_int(const std::string& s, size_t& i, int digits, int& out) {
    if (i + static_cast<size_t>(digits) > s.size()) return false;
    int value = 0;
    for (int k = 0; k < digits; ++k) {
        char c = s[i + static_cast<size_t>(k)];
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        value = value * 10 + (c - '0');
    }
    i += static_cast<size_t>(digits);
    out = value;
    return true;
}

std::optional<long long> parse_duration(const std::string& s, std::string& error) {    if (s.empty() || s[0] != '+') {
        error = "duration must start with '+'";
        return std::nullopt;
    }

    long long total = 0;
    bool parsed_any = false;
    size_t i = 1;

    while (i < s.size()) {
        size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j == i) {
            error = "expected a number in duration '" + s + "'";
            return std::nullopt;
        }

        long long amount = 0;
        for (size_t k = i; k < j; ++k) {
            amount = amount * 10 + (s[k] - '0');
            if (amount > 1000000000LL) {
                error = "duration '" + s + "' is too large";
                return std::nullopt;
            }
        }
        i = j;

        if (i >= s.size()) {
            error = "duration '" + s + "' is missing a unit (s, m, h, d, w)";
            return std::nullopt;
        }

        const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        ++i;

        long long multiplier = 0;
        switch (unit) {
            case 's': multiplier = 1; break;
            case 'm': multiplier = 60; break;
            case 'h': multiplier = 3600; break;
            case 'd': multiplier = 86400; break;
            case 'w': multiplier = 604800; break;
            default:
                error = std::string("unknown duration unit '") + unit +
                        "' (use s, m, h, d or w)";
                return std::nullopt;
        }

        total += amount * multiplier;
        parsed_any = true;
    }

    if (!parsed_any) {
        error = "empty duration";
        return std::nullopt;
    }
    return total;
}

std::optional<std::time_t> parse_absolute(const std::string& s, std::string& error) {
    // Parsed by hand rather than with std::get_time: libstdc++ happily reports
    // success when the input ends before the format is consumed, which would
    // make "2026-10-16" match a "date + clock" format.
    size_t i = 0;
    int year = 0;
    int month = 0;
    int day = 0;

    if (!read_fixed_int(s, i, 4, year)) return std::nullopt;
    if (i >= s.size() || s[i] != '-') return std::nullopt;
    ++i;
    if (!read_fixed_int(s, i, 2, month)) return std::nullopt;
    if (i >= s.size() || s[i] != '-') return std::nullopt;
    ++i;
    if (!read_fixed_int(s, i, 2, day)) return std::nullopt;

    int hour = 9; // a date without a clock means 09:00
    int minute = 0;
    int second = 0;

    if (i < s.size()) {
        if (s[i] != 'T' && s[i] != ' ') {
            error = "unexpected character in '" + s + "'";
            return std::nullopt;
        }
        ++i;

        if (!read_fixed_int(s, i, 2, hour) || i >= s.size() || s[i] != ':') {
            error = "invalid time in '" + s + "'";
            return std::nullopt;
        }
        ++i;
        if (!read_fixed_int(s, i, 2, minute)) {
            error = "invalid time in '" + s + "'";
            return std::nullopt;
        }
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!read_fixed_int(s, i, 2, second)) {
                error = "invalid time in '" + s + "'";
                return std::nullopt;
            }
        }
    }

    if (i != s.size()) {
        error = "unexpected trailing text in '" + s + "'";
        return std::nullopt;
    }
    if (!valid_date(year, month - 1, day)) {
        error = "invalid date '" + s + "'";
        return std::nullopt;
    }
    if (hour > 23 || minute > 59 || second > 60) {
        error = "invalid time '" + s + "'";
        return std::nullopt;
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;

    std::time_t result = std::mktime(&tm);
    if (result == static_cast<std::time_t>(-1)) {
        error = "date out of range: '" + s + "'";
        return std::nullopt;
    }
    return result;
}

bool looks_like_date(const std::string& s) {
    if (s.size() < 10) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    if (s[4] != '-') return false;
    if (!std::isdigit(static_cast<unsigned char>(s[5]))) return false;
    if (!std::isdigit(static_cast<unsigned char>(s[6]))) return false;
    if (s[7] != '-') return false;
    if (!std::isdigit(static_cast<unsigned char>(s[8]))) return false;
    if (!std::isdigit(static_cast<unsigned char>(s[9]))) return false;
    return true;
}

std::optional<std::time_t> parse_phrase(const std::string& s, std::time_t now,
                                        std::string& error) {
    constexpr int kDefaultHour = 9 * 3600;

    size_t space = s.find_first_of(" \t");
    std::string head = space == std::string::npos ? s : s.substr(0, space);
    std::string rest =
        space == std::string::npos ? std::string() : trim(s.substr(space + 1));

    if (head.empty()) return std::nullopt;

    if (head == "now") {
        if (!rest.empty()) {
            error = "unexpected text after 'now': '" + rest + "'";
            return std::nullopt;
        }
        return now;
    }

    if (head == "today" || head == "tomorrow") {
        int seconds = kDefaultHour;
        if (!rest.empty()) {
            auto parsed = parse_time_of_day(rest);
            if (!parsed) {
                error = "invalid time '" + rest + "'";
                return std::nullopt;
            }
            seconds = *parsed;
        }
        std::time_t base = head == "tomorrow" ? add_days(now, 1) : now;
        return local_at(base, seconds);
    }

    if (auto weekday = parse_weekday(head)) {
        int seconds = kDefaultHour;
        if (!rest.empty()) {
            auto parsed = parse_time_of_day(rest);
            if (!parsed) {
                error = "invalid time '" + rest + "'";
                return std::nullopt;
            }
            seconds = *parsed;
        }

        std::tm tm{};
        localtime_r(&now, &tm);
        int offset = (*weekday - tm.tm_wday + 7) % 7;
        std::time_t candidate = local_at(add_days(now, offset), seconds);
        if (candidate <= now) candidate = local_at(add_days(now, offset + 7), seconds);
        return candidate;
    }

    if (rest.empty()) {
        if (auto parsed = parse_time_of_day(head)) {
            std::time_t candidate = local_at(now, *parsed);
            if (candidate <= now) candidate = local_at(add_days(now, 1), *parsed);
            return candidate;
        }
    }

    return std::nullopt;
}

std::string escape_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string unescape_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        char next = s[++i];
        switch (next) {
            case '\\': out += '\\'; break;
            case 'p': out += '|'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            default:
                out += '\\';
                out += next;
                break;
        }
    }
    return out;
}

// Splits on unescaped '|' keeping the escape sequences intact.
std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;

    for (char c : line) {
        if (escaped) {
            current += '\\';
            current += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '|') {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (escaped) current += '\\';
    fields.push_back(current);
    return fields;
}

} // namespace

std::string to_string(Repeat r) {
    switch (r) {
        case Repeat::Daily: return "daily";
        case Repeat::Weekly: return "weekly";
        case Repeat::Monthly: return "monthly";
        case Repeat::None: break;
    }
    return "none";
}

std::optional<Repeat> repeat_from_string(const std::string& text) {
    std::string s = lower(trim(text));
    if (s.empty() || s == "none" || s == "once" || s == "no") return Repeat::None;
    if (s == "daily" || s == "day") return Repeat::Daily;
    if (s == "weekly" || s == "week") return Repeat::Weekly;
    if (s == "monthly" || s == "month") return Repeat::Monthly;
    return std::nullopt;
}

std::string format_time(std::time_t t, const char* fmt) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    if (std::strftime(buf, sizeof buf, fmt, &tm) == 0) return std::string();
    return buf;
}

std::string format_relative(std::time_t due, std::time_t now) {
    long long delta = static_cast<long long>(due) - static_cast<long long>(now);
    if (delta <= 0) return "overdue";

    long long days = delta / 86400;
    long long hours = (delta % 86400) / 3600;
    long long minutes = (delta % 3600) / 60;
    long long seconds = delta % 60;

    std::string out = "in ";
    if (days > 0) {
        out += std::to_string(days) + "d ";
    }
    if (days > 0 || hours > 0) {
        out += std::to_string(hours) + "h ";
    }
    if (days > 0 || hours > 0 || minutes > 0) {
        out += std::to_string(minutes) + "m";
    } else {
        out += std::to_string(seconds) + "s";
    }

    return out;
}

std::optional<std::time_t> parse_when(const std::string& expr, std::time_t now,
                                      std::string& error) {
    error.clear();
    std::string s = trim(expr);
    if (s.empty()) {
        error = "empty time expression";
        return std::nullopt;
    }

    if (s[0] == '@') {
        long long epoch = 0;
        if (!parse_integer(trim(s.substr(1)), epoch) || epoch < 0) {
            error = "invalid unix timestamp after '@': '" + s + "'";
            return std::nullopt;
        }
        return static_cast<std::time_t>(epoch);
    }

    if (s[0] == '+') {
        auto seconds = parse_duration(s, error);
        if (!seconds) return std::nullopt;
        return now + static_cast<std::time_t>(*seconds);
    }

    if (looks_like_date(s)) {
        auto absolute = parse_absolute(s, error);
        if (absolute) return absolute;
        if (error.empty()) error = "invalid date/time '" + s + "'";
        return std::nullopt;
    }

    if (auto phrase = parse_phrase(lower(s), now, error)) return phrase;
    if (!error.empty()) return std::nullopt;

    error = "unrecognized time '" + expr +
            "' (try '18:30', 'tomorrow 09:00', 'fri 17:00', "
            "'2026-10-16 18:30', '+45m' or '@1700000000')";
    return std::nullopt;
}

Calendar::Calendar(std::string path) : path_(std::move(path)) {}

std::string Calendar::default_path() {
    return default_db_path();
}

void Calendar::refresh_stamp() const {
    struct stat st;
    if (stat(path_.c_str(), &st) == 0) {
        mtime_ = st.st_mtime;
        size_ = static_cast<long long>(st.st_size);
    } else {
        mtime_ = 0;
        size_ = -1;
    }
}

bool Calendar::load(std::string& error) {
    events_.clear();
    next_id_ = 1;
    error.clear();

    struct stat st;
    const bool exists = stat(path_.c_str(), &st) == 0;

    std::ifstream in(path_);
    if (!in) {
        if (!exists) {
            refresh_stamp();
            return true; // nothing stored yet
        }
        error = "cannot read " + path_ + ": " + std::strerror(errno);
        return false;
    }

    long long header_next_id = 0;
    std::string line;
    long line_number = 0;

    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;

        if (line[0] == '#') {
            std::string header = trim(line);
            if (header.compare(0, std::strlen(kNextIdTag), kNextIdTag) == 0) {
                long long value = 0;
                if (parse_integer(trim(header.substr(std::strlen(kNextIdTag))), value) &&
                    value > 0) {
                    header_next_id = value;
                }
            }
            continue;
        }

        std::vector<std::string> fields = split_fields(line);
        if (fields.size() != 5) {
            error = path_ + ":" + std::to_string(line_number) + ": expected 5 fields, got " +
                    std::to_string(fields.size());
            return false;
        }

        long long id = 0;
        long long due = 0;
        if (!parse_integer(trim(fields[0]), id) || id <= 0) {
            error = path_ + ":" + std::to_string(line_number) + ": invalid id '" +
                    fields[0] + "'";
            return false;
        }

        auto repeat = repeat_from_string(fields[1]);
        if (!repeat) {
            error = path_ + ":" + std::to_string(line_number) + ": invalid repeat '" +
                    fields[1] + "'";
            return false;
        }

        if (!parse_integer(trim(fields[2]), due)) {
            error = path_ + ":" + std::to_string(line_number) + ": invalid due time '" +
                    fields[2] + "'";
            return false;
        }

        Event e;
        e.id = static_cast<long>(id);
        e.repeat = *repeat;
        e.due = static_cast<std::time_t>(due);
        e.title = unescape_field(fields[3]);
        e.message = unescape_field(fields[4]);
        events_.push_back(std::move(e));
    }

    long long max_id = 0;
    for (const Event& e : events_) max_id = std::max(max_id, static_cast<long long>(e.id));
    next_id_ = static_cast<long>(std::max(header_next_id, max_id + 1));

    refresh_stamp();
    return true;
}

bool Calendar::save(std::string& error) const {
    if (!ensure_parent_dir(path_, error)) return false;

    const std::string tmp = path_ + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
        error = "cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }

    out << kFormatTag << "\n";
    out << kNextIdTag << next_id_ << "\n";

    std::vector<const Event*> ordered;
    ordered.reserve(events_.size());
    for (const Event& e : events_) ordered.push_back(&e);
    std::sort(ordered.begin(), ordered.end(), [](const Event* a, const Event* b) {
        if (a->due != b->due) return a->due < b->due;
        return a->id < b->id;
    });

    for (const Event* e : ordered) {
        out << e->id << '|' << to_string(e->repeat) << '|'
            << static_cast<long long>(e->due) << '|' << escape_field(e->title) << '|'
            << escape_field(e->message) << "\n";
    }

    out.flush();
    if (!out) {
        error = "write failed on " + tmp;
        return false;
    }
    out.close();

    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        error = "cannot replace " + path_ + ": " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }

    refresh_stamp();
    return true;
}

bool Calendar::reload_if_changed(bool& changed, std::string& error) {
    changed = false;
    error.clear();

    struct stat st;
    if (stat(path_.c_str(), &st) != 0) {
        if (mtime_ != 0 || size_ != -1) {
            // The file disappeared; keep what we have in memory.
            mtime_ = 0;
            size_ = -1;
            changed = true;
        }
        return true;
    }

    if (st.st_mtime == mtime_ && static_cast<long long>(st.st_size) == size_) {
        return true;
    }

    if (!load(error)) return false;
    changed = true;
    return true;
}

Event& Calendar::add(const std::string& title, std::time_t due, Repeat repeat,
                     const std::string& message) {
    Event e;
    e.id = next_id_++;
    e.title = title;
    e.message = message;
    e.due = due;
    e.repeat = repeat;
    events_.push_back(std::move(e));
    return events_.back();
}

bool Calendar::remove(long id) {
    const auto before = events_.size();
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [id](const Event& e) { return e.id == id; }),
                  events_.end());
    return events_.size() != before;
}

void Calendar::clear() {
    events_.clear();
}

const Event* Calendar::find(long id) const {
    for (const Event& e : events_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::time_t Calendar::next_due() const {
    std::time_t best = 0;
    for (const Event& e : events_) {
        if (best == 0 || e.due < best) best = e.due;
    }
    return best;
}

std::vector<Event> Calendar::take_due(std::time_t now) {
    std::vector<Event> fired;

    for (auto it = events_.begin(); it != events_.end();) {
        if (it->due > now) {
            ++it;
            continue;
        }

        fired.push_back(*it);

        if (it->repeat == Repeat::None) {
            it = events_.erase(it);
            continue;
        }

        std::time_t next = it->due;
        int guard = 0;
        do {
            switch (it->repeat) {
                case Repeat::Daily: next = add_days(next, 1); break;
                case Repeat::Weekly: next = add_days(next, 7); break;
                case Repeat::Monthly: next = add_months(next, 1); break;
                case Repeat::None: break;
            }
        } while (next <= now && ++guard < 10000);

        it->due = next;
        ++it;
    }

    return fired;
}

} // namespace cal
