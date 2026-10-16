# calendly

Schedule events from the terminal and get a desktop notification when they are due.
Written in C++17 with no third-party libraries.

```
$ calendly add "stand up and stretch" --at 18:30 --repeat daily
added #1  2026-10-16 18:30:00  (in 2h 5m)  [daily]  stand up and stretch

$ calendly start
daemon started (pid 20431)
  db:  /home/you/.local/share/calendly/events.db
  log: /home/you/.local/state/calendly/calendly.log

$ calendly list
ID    WHEN                 IN           REPEAT    TITLE
3     2026-10-16 23:45:00  in 4m        daily     sleep
2     2026-10-17 00:14:00  in 33m       none      deploy (freeze is over)
1     2026-10-17 18:30:00  in 18h 49m   daily     stand up and stretch
```

- One-shot and repeating events (daily / weekly / monthly)
- Natural time input: `18:30`, `tomorrow 09:00`, `fri 17:00`, `+45m`, `2026-10-16 18:30`
- A small background daemon that watches its event file, so nothing needs restarting
- Desktop notifications through `notify-send`, with a stdout fallback on headless machines
- Plain-text storage, a `--json` output for scripting, no dependencies to link

**Contents:** [Requirements](#requirements) · [Install](#install) ·
[Usage](#usage) · [Commands](#commands) · [Time formats](#time-formats) ·
[Run it automatically](#run-it-automatically) · [Files](#files) ·
[Troubleshooting](#troubleshooting)

## Requirements

| | |
| --- | --- |
| Compiler | Any C++17 compiler (GCC 8+, Clang 6+) |
| Build tool | CMake 3.16 or newer |
| Runtime | `notify-send` for desktop notifications — optional, see below |

There is nothing to link against: the daemon runs `notify-send` as a child
process instead of using libnotify. `notify-send` ships with the desktop itself
on most systems; if `notify-send --version` fails, install it:

| Distribution | Package |
| --- | --- |
| Debian, Ubuntu, Mint | `sudo apt install libnotify-bin` |
| Fedora, RHEL | `sudo dnf install libnotify` |
| Arch, Manjaro | `sudo pacman -S libnotify` |
| openSUSE | `sudo zypper install libnotify-tools` |

Without it `calendly` still works — alerts are printed on stdout and appended to
the log instead.

## Install

### From source, for the current user (recommended)

```sh
git clone https://github.com/jfdiazbus/Tools.git
cd Tools
cmake -S . -B build
cmake --build build -j
cmake --install build --prefix ~/.local
```

This copies the binary to `~/.local/bin/calendly`. Make sure that directory is on
your `PATH` — it is by default on most distributions. If `calendly version` says
`command not found`, add this to `~/.bashrc`, `~/.zshrc` or your shell's profile:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

then open a new terminal.

### While developing

Symlink instead of copying, so every rebuild updates the command you run:

```sh
mkdir -p ~/.local/bin
ln -sfn "$PWD/build/calendly" ~/.local/bin/calendly
```

### System-wide, for every user on the machine

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build --prefix /usr/local
```

The binary lands in `/usr/local/bin/calendly`, which is already on everyone's
`PATH`. Each user still gets their own events, log and daemon — only the program
is shared.

### Verify

```sh
calendly version     # calendly 0.1.0
calendly notify-test # should pop up a desktop notification
```

### Uninstall

```sh
rm ~/.local/bin/calendly                              # or: sudo rm /usr/local/bin/calendly
rm -rf ~/.local/share/calendly ~/.local/state/calendly  # your events, log and pid file
```

## Usage

```sh
# Schedule something
calendly add "deploy the release" --in 45m --message "freeze is over"
calendly add "week review" --at "fri 17:00" --repeat weekly
calendly add "take the pills" --at "tomorrow 08:00"

# Start the daemon once per session, then forget about it
calendly start

# Inspect and manage
calendly list
calendly status
calendly remove 2
calendly stop
```

**The daemon has to be running when an event comes due.** `calendly add` only
writes the event to a file; the daemon is what watches the clock and fires it.
Run `calendly start` once after logging in — see
[Run it automatically](#run-it-automatically) to have that happen by itself.

The daemon re-reads the event file while it runs, so events added, changed or
removed at any moment are picked up within a second — no restart needed.

## Commands

| Command | Description |
| --- | --- |
| `calendly add <title> --at <when>` | Schedule a new event |
| `calendly list [--json]` | List scheduled events, earliest first |
| `calendly remove <id>...` | Delete events by id (several ids allowed) |
| `calendly clear --yes` | Delete every event |
| `calendly start` | Start the daemon in the background |
| `calendly stop` | Stop the daemon (SIGTERM, then SIGKILL) |
| `calendly daemon` | Run the daemon in the foreground (for systemd, containers, debugging) |
| `calendly status [--json]` | Daemon state plus the next events |
| `calendly notify-test` | Send a test notification and report which channel was used |
| `calendly help` (`-h`) / `calendly version` (`-v`) | Usage / version |

### `add` options

| Option | Description |
| --- | --- |
| `--at <when>` | When the event is due (required) |
| `--in <duration>` | Shortcut for `--at +<duration>`, e.g. `--in 45m` |
| `--repeat <none\|daily\|weekly\|monthly>` | Repeat the event (default `none`) |
| `--message <text>` | Extra text shown with the notification |

### Daemon options

| Option | Description |
| --- | --- |
| `--interval <seconds>` | How often the event file is re-checked (default `1`) |
| `--once` | Fire everything that is due, then exit (good for cron) |
| `--stdout` | Print alerts instead of using `notify-send` |
| `--force` | Start even when another daemon is already running |

### Options accepted by every command

| Option | Description |
| --- | --- |
| `--db <path>` | Event file to use |
| `--log <path>` | Log file for the daemon |
| `--pidfile <path>` | Pid file used by `start`/`stop`/`status` |

`--at` and `--in` also have the short forms `-a` and `-i`, `--message` is `-m`,
`--repeat` is `-r`, `--db`/`--log`/`--pidfile` are `-d`/`-l`/`-p`, and
`--interval` is `-n`.

### Exit codes

`0` success · `1` runtime problem (nothing to remove, daemon not running, write
failed) · `2` bad usage (unknown command, missing `--at`, unparsable time).

## Time formats

| Input | Meaning |
| --- | --- |
| `18:30` | Today at 18:30, or tomorrow when that already passed |
| `18:30:45` | Same, with seconds |
| `today 18:30` | Today, even when the time already passed |
| `tomorrow 09:00` | Tomorrow (`09:00` when the time is omitted) |
| `tomorrow` | Tomorrow at 09:00 |
| `mon 17:00` | The next such weekday (`09:00` when omitted) |
| `2026-10-16 18:30` | An absolute date/time (`2026-10-16T18:30:00` also works) |
| `2026-10-16` | A date, at 09:00 |
| `+45m` `+2h30m` `+1d` | A duration from now — units `s`, `m`, `h`, `d`, `w` |
| `@1700000000` | A raw unix timestamp |

Everything is interpreted in your local time zone. **Quote values that contain a
space**, otherwise the shell splits them and the extra words end up in the title:

```sh
calendly add "week review" --at "fri 17:00"    # correct
calendly add "week review" --at fri 17:00      # wrong: title becomes "week review 17:00"
```

Repeating events move to their next occurrence when they fire. If the daemon was
off for a while, a repeating event fires once and jumps to the next future
occurrence instead of replaying the whole backlog. Monthly events clamp short
months: Jan 31 becomes Feb 28 (or 29).

## Notifications

When an event comes due the daemon

1. runs `notify-send --app-name=calendly --urgency=normal --icon=appointment-soon <title> [message]`, and
2. always appends a line to the log file.

If there is no `DISPLAY`/`WAYLAND_DISPLAY`, or `notify-send` is missing, the alert
is printed on stdout instead (with a terminal bell). A background daemon has its
stdout redirected to the log file, so nothing is lost either way:

```
[2026-10-16 23:45:00] fired #3 2026-10-16 23:45:00 sleep
[2026-10-16 23:45:00] rescheduled #3 to 2026-10-17 23:45:00
```

Point `CALENDLY_NOTIFY_CMD` at your own program to use something other than
`notify-send` (for example a wrapper that also plays a sound).

## Run it automatically

`calendly start` is per-session: the daemon dies with the terminal session, so
you have to start it again after every reboot. Set up one of the options below
to have it come up by itself.

Only one daemon may run per event file. If you set one of these up, do not also
run `calendly start` — the second daemon refuses to start with
`already running (pid ...)`. That protection is deliberate, but it is easy to
mistake for a broken service.

### systemd user service (recommended)

The daemon runs in the foreground, so systemd supervises it and can restart it
if it ever crashes. Save this as
`~/.config/systemd/user/calendly.service`:

```ini
[Unit]
Description=calendly event notification daemon

[Service]
ExecStart=%h/.local/bin/calendly daemon
Restart=on-failure

[Install]
WantedBy=default.target
```

```sh
systemctl --user daemon-reload
systemctl --user enable --now calendly
systemctl --user status calendly     # expect: active (running)
journalctl --user -u calendly -f     # live view of alerts and errors
```

`enable` starts it at every login, `--now` starts it immediately, and `%h`
expands to the home directory, so the same unit works for every user. Optional
hardening: add `RestartSec=5` under `Restart=` so a real startup problem does
not turn into a burst of restarts.

Once the service is in charge, manage the daemon with systemd rather than with
`calendly`:

```sh
systemctl --user stop calendly           # instead of: calendly stop
systemctl --user restart calendly        # after changing CALENDLY_* variables
systemctl --user disable --now calendly  # stop and remove from autostart
```

`calendly stop` still works, but systemd then no longer knows the stop was
intentional; prefer `systemctl`.

#### Can the service still show notifications?

A user service inherits the environment of the *user manager*, not of your
terminal. Most desktops import `DISPLAY`/`WAYLAND_DISPLAY` into it, in which case
notifications work exactly as they do from a shell. The daemon records the
channel it got when it started:

```sh
grep "daemon started" ~/.local/state/calendly/calendly.log | tail -1
# [2026-10-17 00:07:49] daemon started pid=2392040 db=/home/.../events.db notify=desktop
```

`notify=desktop` means `notify-send` is reachable and alerts pop up normally. If
it says `notify=stdout`, alerts are only reaching the journal and the log file —
give the service the session variables explicitly:

```ini
[Service]
ExecStart=%h/.local/bin/calendly daemon
Restart=on-failure
Environment=DISPLAY=:0
Environment=WAYLAND_DISPLAY=wayland-1
```

or use the autostart entry below, which always inherits the live session.

### Desktop autostart (when systemd is not an option)

Save this as `~/.config/autostart/calendly.desktop`, replacing `/home/you` with
your real home directory (the `Exec` line needs an absolute path):

```ini
[Desktop Entry]
Type=Application
Name=calendly
Comment=Fire due events and show notifications
Exec=/home/you/.local/bin/calendly daemon
Terminal=false
```

It starts with your next login and inherits the session's `DISPLAY`/
`WAYLAND_DISPLAY`, so notifications show up normally.

### Cron, without a long-running daemon

```sh
* * * * * /home/you/.local/bin/calendly daemon --once --stdout >> /tmp/calendly-cron.log 2>&1
```

Cron has no access to your desktop either, so this prints to the log rather than
showing a notification. `--once` fires what is due and exits immediately.

## Files

| File | Default location |
| --- | --- |
| Events | `$XDG_DATA_HOME/calendly/events.db` — `~/.local/share/calendly/events.db` |
| Log | `$XDG_STATE_HOME/calendly/calendly.log` — `~/.local/state/calendly/calendly.log` |
| Pid file | `$XDG_STATE_HOME/calendly/calendly.pid` |

The event file is a small, human-readable text file. Titles and messages may
contain `|`, newlines and backslashes — they are escaped. Every edit is written
to a temporary file and renamed into place, so the daemon never reads a
half-written file.

```
# calendly events -- format v1 (id|repeat|due_epoch|title|message)
# next_id=4
3|daily|1792286700|sleep|
2|none|1792188432|deploy the release|freeze is over
1|daily|1792254600|stand up and stretch|
```

### Environment

| Variable | Effect |
| --- | --- |
| `CALENDLY_DB` | Same as `--db` |
| `CALENDLY_LOG` | Same as `--log` |
| `CALENDLY_PIDFILE` | Same as `--pidfile` |
| `CALENDLY_NOTIFY_CMD` | Program used for notifications (default `notify-send`) |
| `CALENDLY_FORCE_STDOUT=1` | Never use the desktop, always print |
| `CALENDLY_FORCE_DESKTOP=1` | Notify even without `DISPLAY`/`WAYLAND_DISPLAY` |

`XDG_DATA_HOME` and `XDG_STATE_HOME` move the defaults above.

### Limitations

Writes are atomic, but a read-modify-write is not locked, so an `add` that lands
at the exact moment the daemon saves a fired event can be lost. That window is a
few milliseconds wide, and one daemon per event file is the intended setup.

## Troubleshooting

**`calendly: command not found`** — the binary is not on your `PATH`. Use
`cmake --install build --prefix ~/.local` and add `~/.local/bin` to `PATH` as
shown in [Install](#install).

**Nothing happened at the event time** — the daemon was probably not running.
Check with `calendly status` and start it with `calendly start`. Look at the log
file (`~/.local/state/calendly/calendly.log`) to see what the daemon did.

**No desktop notification, but the log shows `[stdout]`** — `notify-send` is
missing or the daemon has no display. Install the `notify-send` package for your
distribution, and run `calendly notify-test` in a normal terminal to see which
channel is used.

**`calendly start` says "already running"** — a daemon is already there. If you
run the systemd service, that is expected: use `systemctl --user status calendly`
instead, and `systemctl --user stop calendly` when you want it gone. Otherwise
use `calendly stop`, or `calendly start --force` if you really want a second one
(you do not; two daemons fire every event twice).

**`calendly.service` failed with "already running (pid ...)"** — you had started
a daemon by hand before enabling the service, so the service could not take over
the pid file. Hand control to systemd:

```sh
calendly stop
systemctl --user reset-failed calendly
systemctl --user start calendly
```

After that, do not run `calendly start` again while the service is enabled.

**The alert is in the log but no popup appeared** — `notify-send` succeeded, but
something still has to draw the notification. Check that a notification daemon
owns the well-known name:

```sh
busctl --user list | grep org.freedesktop.Notifications
```

If nothing shows up, start your notification daemon (GNOME's shell, `mako`,
`dunst`, `swaync`, ...) — `calendly` only asks it to display the alert.

**An event fired immediately after being added** — the time you gave is already
in the past. `18:30` rolls over to tomorrow, but `today 18:30` or an absolute
date does not, so those fire as soon as the daemon notices them.

**Times are off by a few hours** — everything is local time, so check your
`TZ`/system clock rather than the tool.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The dependency-free test binary covers time parsing, storage round-tripping
(including escaping), firing and rescheduling of repeating events, and live
reloads.

## Layout

```
include/calendly.hpp   events, time parsing, persistence
include/notifier.hpp   desktop notification + logging
include/paths.hpp      XDG directories and path helpers
src/                   implementations and the CLI (main.cpp)
tests/                 dependency-free test binary
```

## License

MIT — see [LICENSE](LICENSE).
