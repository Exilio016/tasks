#include "gui.hpp"
#include "task.hpp"

#include <SQLiteCpp/Database.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace {

// Advisory exclusive lock on a sibling ".lock" file, released automatically
// by the kernel when the fd is closed (including abnormal termination). We
// intentionally don't lock the SQLite file itself so SQLite's own locking
// machinery is left untouched.
class SingleInstanceLock {
public:
    SingleInstanceLock() = default;
    SingleInstanceLock(const SingleInstanceLock &) = delete;
    SingleInstanceLock &operator=(const SingleInstanceLock &) = delete;

    ~SingleInstanceLock() {
        if (m_fd >= 0) ::close(m_fd);
    }

    // Tries to acquire the lock. Returns true on success. On failure sets
    // `error` to a human-readable reason.
    bool acquire(const std::string &path, std::string &error) {
        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_fd < 0) {
            error = std::format("cannot open lock file {}: {}",
                                path, std::strerror(errno));
            return false;
        }
        if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
            int err = errno;
            ::close(m_fd);
            m_fd = -1;
            if (err == EWOULDBLOCK) {
                error = "another instance is already running";
            } else {
                error = std::format("flock failed: {}", std::strerror(err));
            }
            return false;
        }
        return true;
    }

private:
    int m_fd = -1;
};

}  // namespace

int main(int argc, char *argv[]) {
    auto state_home = std::getenv("XDG_STATE_HOME");
    std::string state_dir;
    if (!state_home) {
        auto home = std::getenv("HOME");
        state_dir = std::format("{0}/.local/state/tasks", home);
    } else {
        state_dir = std::format("{0}/tasks", state_home);
    }
    auto database = std::format("{0}/sqlite.db", state_dir);
    auto lockfile = std::format("{0}/sqlite.db.lock", state_dir);

    if (!std::filesystem::exists(state_dir)) {
        std::filesystem::create_directories(state_dir);
    }

    // Acquire before opening the DB so a second instance fails fast without
    // touching SQLite at all.
    SingleInstanceLock lock;
    std::string lock_error;
    if (!lock.acquire(lockfile, lock_error)) {
        std::cerr << "Tasks: " << lock_error << "\n";
        return 1;
    }

    try {
        SQLite::Database db{database, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE};
        Task::create_tables(db);
        return gui::run(argc, argv, db);
    } catch (std::exception &e) {
        std::cout << "Error: " << e.what() << std::endl;
        return 1;
    }
}
