#pragma once

#include <string>

namespace cal {

// Expands a leading "~" to $HOME. "~user" is not supported and is returned as is.
std::string expand_user(const std::string& path);

// The directory part of `path`, or "" when there is none.
std::string parent_dir(const std::string& path);

// Creates the parent directories of `path` if they do not exist yet.
bool ensure_parent_dir(const std::string& path, std::string& error);

// $XDG_DATA_HOME or ~/.local/share
std::string data_dir();

// $XDG_STATE_HOME or ~/.local/state
std::string state_dir();

std::string default_db_path();
std::string default_log_path();
std::string default_pid_path();

// Moves data left behind by the previous "calendar" name to the "calendly"
// locations. Does nothing when the new location already holds a file.
void migrate_legacy_paths();

} // namespace cal
