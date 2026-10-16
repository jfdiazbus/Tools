#pragma once

#include <string>

namespace cal {

struct NotifyOptions {
    bool desktop = true;   // false forces stdout-only delivery
    std::string command;   // override for the notification program
};

// Path of the notify-send compatible program that will be used, or "" when
// none is available.
std::string default_notify_command();

// True when a desktop session and a notification program are both available.
bool desktop_available(const std::string& command = "");

// Sends a desktop notification. Returns false when nothing was sent, in which
// case the caller should fall back to printing on stdout.
bool notify(const std::string& title, const std::string& body,
            const NotifyOptions& opts = NotifyOptions{});

// Appends "[timestamp] text" to `path`, creating the file when needed.
void log_line(const std::string& path, const std::string& text);

std::string log_timestamp();

} // namespace cal
