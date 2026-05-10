# Tasks

A small desktop task manager written in C++23 using GTK4 and SQLite. It tracks
the time you spend working on each task, with per-day breakdowns, and persists
everything in a local SQLite database.

## Features

- Create, update, and remove tasks
- Track task status: *Not Started*, *Working*, *Paused*, *Done*
- Accumulated total work time plus per-calendar-day work time
- Filter tasks by status (All / Pending / Working / Paused / Done / Not Started)
- Single-instance enforcement via an advisory `flock` lock

## Requirements

- CMake ≥ 3.20
- A C++23 compiler (recent GCC or Clang)
- [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp)
- GTK 4 (with `pkg-config`)
- Ninja

On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libsqlitecpp-dev libgtk-4-dev ninja
```

## Build

```bash
cmake --workflow --preset x64-linux-gcc
```

The binary is produced at `build/tasks`. Run it with:

```bash
./build/tasks
```

## Data location

The SQLite database and lock file live under the XDG state directory:

- `$XDG_STATE_HOME/tasks/sqlite.db` if `XDG_STATE_HOME` is set
- `$HOME/.local/state/tasks/sqlite.db` otherwise

A sibling `sqlite.db.lock` file is used for single-instance locking; it is
created automatically and released when the process exits.

## Project layout

```
.
├── CMakeLists.txt              CMake build configuration
├── main.cpp                    Entry point + single-instance lock + DB bootstrap
├── gui.hpp / gui.cpp           GTK4 user interface
└──  task.hpp / task.cpp         Task model, persistence, per-day work time
```

## License

See [LICENSE](LICENSE).
