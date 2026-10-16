#include "calendly.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void report(bool ok, const char* expression, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
}

#define CHECK(expr) report((expr), #expr, __FILE__, __LINE__)

std::string temp_db(const std::string& name) {
    static const std::string base = [] {
        std::filesystem::path root = std::filesystem::temp_directory_path();
        std::string dir = (root / ("calendly_test_" + std::to_string(getpid()))).string();
        std::filesystem::create_directories(dir);
        return dir;
    }();
    return base + "/" + name;
}

std::time_t local_at(std::time_t day, int hour, int minute) {
    std::tm tm{};
    localtime_r(&day, &tm);
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

void test_parse_durations() {
    const std::time_t now = 1700000000;
    std::string error;

    auto ninety = cal::parse_when("+90m", now, error);
    CHECK(ninety.has_value());
    CHECK(ninety && *ninety == now + 5400);

    auto compound = cal::parse_when("+1h30m", now, error);
    CHECK(compound && *compound == now + 5400);

    auto mixed = cal::parse_when("+1d2h", now, error);
    CHECK(mixed && *mixed == now + 86400 + 7200);

    auto week = cal::parse_when("+1w", now, error);
    CHECK(week && *week == now + 604800);

    auto epoch = cal::parse_when("@1700000000", now, error);
    CHECK(epoch && *epoch == 1700000000);

    CHECK(!cal::parse_when("+5x", now, error).has_value());
    CHECK(!error.empty());
    CHECK(!cal::parse_when("+5", now, error).has_value());
    CHECK(!cal::parse_when("", now, error).has_value());
    CHECK(!cal::parse_when("lunch time", now, error).has_value());
}

void test_parse_clock_times() {
    std::string error;
    const std::time_t now = local_at(std::time(nullptr), 10, 0);

    auto later = cal::parse_when("18:30", now, error);
    CHECK(later.has_value());
    if (later) {
        CHECK(*later == local_at(now, 18, 30));
        CHECK(*later > now);
    }

    // A time that already passed rolls over to tomorrow.
    auto past = cal::parse_when("07:15", now, error);
    CHECK(past.has_value());
    if (past) {
        CHECK(*past > now);
        std::tm tm{};
        localtime_r(past ? &*past : &now, &tm);
        CHECK(tm.tm_hour == 7);
        CHECK(tm.tm_min == 15);
    }

    auto with_seconds = cal::parse_when("18:30:45", now, error);
    CHECK(with_seconds.has_value());
    if (with_seconds) CHECK(*with_seconds == local_at(now, 18, 30) + 45);

    CHECK(!cal::parse_when("25:00", now, error).has_value());
    CHECK(!cal::parse_when("18:75", now, error).has_value());
}

void test_parse_phrases() {
    std::string error;
    const std::time_t now = local_at(std::time(nullptr), 10, 0);

    auto today = cal::parse_when("today 08:00", now, error);
    CHECK(today && *today == local_at(now, 8, 0));

    auto tomorrow = cal::parse_when("tomorrow 09:00", now, error);
    CHECK(tomorrow.has_value());
    if (tomorrow) {
        std::tm tm{};
        localtime_r(&*tomorrow, &tm);
        CHECK(tm.tm_hour == 9);
        CHECK(tm.tm_min == 0);
        CHECK(*tomorrow > now);
    }

    auto tomorrow_default = cal::parse_when("tomorrow", now, error);
    CHECK(tomorrow_default.has_value());
    if (tomorrow_default) {
        std::tm tm{};
        localtime_r(&*tomorrow_default, &tm);
        CHECK(tm.tm_hour == 9);
    }

    auto sunday = cal::parse_when("sun 09:00", now, error);
    CHECK(sunday.has_value());
    if (sunday) {
        std::tm tm{};
        localtime_r(&*sunday, &tm);
        CHECK(tm.tm_wday == 0);
        CHECK(tm.tm_hour == 9);
        CHECK(*sunday > now);
    }

    auto monday = cal::parse_when("monday 17:30", now, error);
    CHECK(monday.has_value());
    if (monday) {
        std::tm tm{};
        localtime_r(&*monday, &tm);
        CHECK(tm.tm_wday == 1);
        CHECK(tm.tm_hour == 17);
        CHECK(tm.tm_min == 30);
    }
}

void test_parse_dates() {
    std::string error;
    const std::time_t now = 1700000000;

    auto date_time = cal::parse_when("2026-10-16 18:30", now, error);
    CHECK(date_time.has_value());
    if (date_time) CHECK(*date_time == local_at(*date_time, 18, 30));

    auto iso = cal::parse_when("2026-10-16T18:30:00", now, error);
    CHECK(iso.has_value());
    CHECK(date_time && iso && *iso == *date_time);

    auto date_only = cal::parse_when("2026-10-16", now, error);
    CHECK(date_only.has_value());
    if (date_only) {
        std::tm tm{};
        localtime_r(&*date_only, &tm);
        CHECK(tm.tm_year + 1900 == 2026);
        CHECK(tm.tm_mon == 9);
        CHECK(tm.tm_mday == 16);
        CHECK(tm.tm_hour == 9);
    }

    CHECK(!cal::parse_when("2026-02-31", now, error).has_value());
    CHECK(!error.empty());
}

void test_repeat_parsing() {
    CHECK(cal::repeat_from_string("daily") == cal::Repeat::Daily);
    CHECK(cal::repeat_from_string("WEEKLY") == cal::Repeat::Weekly);
    CHECK(cal::repeat_from_string("monthly") == cal::Repeat::Monthly);
    CHECK(cal::repeat_from_string("none") == cal::Repeat::None);
    CHECK(!cal::repeat_from_string("hourly").has_value());

    CHECK(cal::to_string(cal::Repeat::Daily) == "daily");
    CHECK(cal::to_string(cal::Repeat::None) == "none");
}

void test_storage_round_trip() {
    const std::string path = temp_db("round_trip.db");
    std::remove(path.c_str());

    cal::Calendar calendar(path);
    std::string error;
    CHECK(calendar.load(error));
    CHECK(calendar.events().empty());

    const std::string tricky = "pipe|newline\nback\\slash";
    const std::time_t due = 1700000000;
    cal::Event& added = calendar.add(tricky, due, cal::Repeat::Weekly, "note|with pipe");
    CHECK(added.id == 1);
    CHECK(calendar.add("second", due + 60, cal::Repeat::None, "").id == 2);
    CHECK(calendar.save(error));

    cal::Calendar reloaded(path);
    CHECK(reloaded.load(error));
    CHECK(reloaded.events().size() == 2);
    if (reloaded.events().size() == 2) {
        CHECK(reloaded.events()[0].title == tricky);
        CHECK(reloaded.events()[0].message == "note|with pipe");
        CHECK(reloaded.events()[0].due == due);
        CHECK(reloaded.events()[0].repeat == cal::Repeat::Weekly);
    }

    // Ids must not be reused after a reload.
    cal::Event& third = reloaded.add("third", due + 120, cal::Repeat::None, "");
    CHECK(third.id == 3);

    CHECK(reloaded.remove(2));
    CHECK(!reloaded.remove(99));
    CHECK(reloaded.events().size() == 2);
    CHECK(reloaded.save(error));

    cal::Calendar again(path);
    CHECK(again.load(error));
    CHECK(again.events().size() == 2);
    CHECK(again.find(2) == nullptr);
    CHECK(again.find(1) != nullptr);
}

void test_take_due() {
    const std::string path = temp_db("take_due.db");
    std::remove(path.c_str());

    const std::time_t now = 1700000000;
    cal::Calendar calendar(path);
    std::string error;
    CHECK(calendar.load(error));

    calendar.add("one shot", now - 10, cal::Repeat::None, "");
    calendar.add("daily", now - 10, cal::Repeat::Daily, "");
    calendar.add("future", now + 3600, cal::Repeat::None, "");

    std::vector<cal::Event> fired = calendar.take_due(now);
    CHECK(fired.size() == 2);

    // The one-shot event is gone, the daily one moved past `now`.
    CHECK(calendar.events().size() == 2);
    const cal::Event* daily = calendar.find(2);
    CHECK(daily != nullptr);
    if (daily) CHECK(daily->due > now);
    CHECK(calendar.find(1) == nullptr);
    CHECK(calendar.find(3) != nullptr);

    // A second pass at the same instant must not fire anything.
    CHECK(calendar.take_due(now).empty());
}

void test_monthly_repeat_clamps_day() {
    const std::string path = temp_db("monthly.db");
    std::remove(path.c_str());

    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 0; // January
    tm.tm_mday = 31;
    tm.tm_hour = 12;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    const std::time_t january_31 = std::mktime(&tm);

    cal::Calendar calendar(path);
    std::string error;
    CHECK(calendar.load(error));
    calendar.add("monthly", january_31, cal::Repeat::Monthly, "");

    std::vector<cal::Event> fired = calendar.take_due(january_31);
    CHECK(fired.size() == 1);

    const cal::Event* next = calendar.find(1);
    CHECK(next != nullptr);
    if (next) {
        std::tm out{};
        localtime_r(&next->due, &out);
        CHECK(out.tm_mon == 1);            // February
        CHECK(out.tm_mday == 28);          // clamped, 2026 is not a leap year
        CHECK(next->due > january_31);
    }
}

void test_reload_if_changed() {
    const std::string path = temp_db("reload.db");
    std::remove(path.c_str());

    cal::Calendar calendar(path);
    std::string error;
    CHECK(calendar.load(error));

    bool changed = false;
    CHECK(calendar.reload_if_changed(changed, error));
    CHECK(!changed);

    {
        std::ofstream out(path, std::ios::trunc);
        out << "# next_id=2\n";
        out << "1|none|1700000000|external event|\n";
    }

    CHECK(calendar.reload_if_changed(changed, error));
    CHECK(changed);
    CHECK(calendar.events().size() == 1);
    if (!calendar.events().empty()) {
        CHECK(calendar.events()[0].title == "external event");
    }

    changed = true;
    CHECK(calendar.reload_if_changed(changed, error));
    CHECK(!changed);
}

} // namespace

int main() {
    test_parse_durations();
    test_parse_clock_times();
    test_parse_phrases();
    test_parse_dates();
    test_repeat_parsing();
    test_storage_round_trip();
    test_take_due();
    test_monthly_repeat_clamps_day();
    test_reload_if_changed();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
