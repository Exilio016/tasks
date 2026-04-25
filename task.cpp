#include "task.hpp"
#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <chrono>
#include <ctime>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using time_point = std::chrono::system_clock::time_point;
using duration = std::chrono::system_clock::duration;
using hours_double = std::chrono::duration<double, std::ratio<3600>>;

namespace {

// Convert a system time_point to the local calendar day it falls on.
Task::date to_local_day(time_point tp) {
    auto zt = std::chrono::zoned_time{std::chrono::current_zone(), tp};
    auto local_now = zt.get_local_time();
    auto days = std::chrono::floor<std::chrono::days>(local_now);
    return Task::date{days};
}

// Encode a calendar day as an integer number of days since the local epoch,
// suitable for a stable primary key in SQLite.
int64_t date_to_day_number(Task::date d) {
    std::chrono::local_days ld{d};
    return ld.time_since_epoch().count();
}

Task::date day_number_to_date(int64_t n) {
    std::chrono::local_days ld{std::chrono::days{n}};
    return Task::date{ld};
}

Task::date today_local() {
    return to_local_day(std::chrono::system_clock::now());
}

duration hours_to_duration(double hours) {
    return std::chrono::duration_cast<duration>(hours_double{hours});
}

double duration_to_hours(duration d) {
    return std::chrono::duration_cast<hours_double>(d).count();
}

duration query_today_work_time(SQLite::Database& db, long task_id) {
    if (task_id < 0) return duration::zero();
    SQLite::Statement q{
        db, "SELECT work_time FROM TASK_WORK_TIME_BY_DAY WHERE task_id = ? AND day = ?"};
    q.bind(1, static_cast<int64_t>(task_id));
    q.bind(2, date_to_day_number(today_local()));
    if (q.executeStep()) {
        return hours_to_duration(q.getColumn(0).getDouble());
    }
    return duration::zero();
}

} // namespace

Task::Task(std::string_view name) noexcept:
    m_id(-1),
    m_name(name),
    m_status(Task::Status::NotStarted),
    m_created_date(std::chrono::system_clock::now()),
    m_started_date(time_point{}),
    m_completed_date(time_point{}),
    m_work_time(duration::zero()),
    m_today_work_time(duration::zero()) { }

Task::Task(long id, std::string_view name, Status status,
        time_point create_date, time_point started_date, time_point completed_date,
        duration work_time, duration today_work_time) noexcept:
    m_id(id),
    m_name(name),
    m_status(status),
    m_created_date(create_date),
    m_started_date(started_date),
    m_completed_date(completed_date),
    m_work_time(work_time),
    m_today_work_time(today_work_time) { }

void Task::save(SQLite::Database& db) {
    if (m_id < 0) {
        SQLite::Statement query { db, "INSERT INTO TASKS (name, status, created_date, started_date, completed_date, work_time) VALUES (?, ?, ?, ?, ?, ?)" };
        query.bind(1, m_name);
        query.bind(2, static_cast<unsigned int>(m_status));
        query.bind(3, std::chrono::system_clock::to_time_t(m_created_date));
        query.bind(4, std::chrono::system_clock::to_time_t(m_started_date));
        query.bind(5, std::chrono::system_clock::to_time_t(m_completed_date));
        query.bind(6, duration_to_hours(m_work_time));

        query.exec();
        m_id = db.getLastInsertRowid();
    }
    else {
        SQLite::Statement query { db, "UPDATE TASKS set name = ?, status = ?, created_date = ?, started_date = ?, completed_date = ?, work_time = ? where id = ?" };
        query.bind(1, m_name);
        query.bind(2, static_cast<unsigned int>(m_status));
        query.bind(3, std::chrono::system_clock::to_time_t(m_created_date));
        query.bind(4, std::chrono::system_clock::to_time_t(m_started_date));
        query.bind(5, std::chrono::system_clock::to_time_t(m_completed_date));
        query.bind(6, duration_to_hours(m_work_time));
        query.bind(7, m_id);

        query.exec();
    }

    // Persist today's per-day accumulated work time. We attribute the whole
    // session to the day on which save() is invoked (see start/pause/complete
    // for the simplification that avoids splitting sessions across midnight).
    if (m_today_work_time > duration::zero()) {
        SQLite::Statement upsert { db,
            "INSERT INTO TASK_WORK_TIME_BY_DAY (task_id, day, work_time) "
            "VALUES (?, ?, ?) "
            "ON CONFLICT(task_id, day) DO UPDATE SET work_time = excluded.work_time" };
        upsert.bind(1, static_cast<int64_t>(m_id));
        upsert.bind(2, date_to_day_number(today_local()));
        upsert.bind(3, duration_to_hours(m_today_work_time));
        upsert.exec();
    }
}

std::map<Task::date, duration> Task::get_work_time_by_day(SQLite::Database& db) const {
    std::map<date, duration> result;
    if (m_id < 0) return result;

    SQLite::Statement q { db,
        "SELECT day, work_time FROM TASK_WORK_TIME_BY_DAY WHERE task_id = ? ORDER BY day" };
    q.bind(1, static_cast<int64_t>(m_id));
    while (q.executeStep()) {
        auto day_num = q.getColumn(0).getInt64();
        auto hours = q.getColumn(1).getDouble();
        result.emplace(day_number_to_date(day_num), hours_to_duration(hours));
    }
    return result;
}


constexpr std::string render_status_filter(Task::StatusFilter filter) {
    switch (filter) {
        case Task::StatusFilter::All:
            return "1 = 1";
        case Task::StatusFilter::Done:
            return std::format("status = {0}", static_cast<int>(Task::Status::Done));
        case Task::StatusFilter::Paused:
            return std::format("status = {0}", static_cast<int>(Task::Status::Paused));
        case Task::StatusFilter::Pending:
            return std::format("status != {0}", static_cast<int>(Task::Status::Done));
        case Task::StatusFilter::NotStarted:
            return std::format("status = {0}", static_cast<int>(Task::Status::NotStarted));
        case Task::StatusFilter::Working:
            return std::format("status = {0}", static_cast<int>(Task::Status::Working));
    }
    throw std::runtime_error { std::format("Unknown StatusFilter {0}",  static_cast<int>(filter)) };
}

std::vector<Task> Task::get_tasks(SQLite::Database& db, Task::StatusFilter filter) {
    std::vector<Task> tasks;
    tasks.reserve(10);

    const auto clause = render_status_filter(filter);
    const auto today_num = date_to_day_number(today_local());

    // LEFT JOIN pulls today's accumulated per-day time in a single query so we
    // avoid an N+1 when listing tasks.
    SQLite::Statement query {db, std::format(
        "SELECT t.id, t.name, t.status, t.created_date, t.started_date, t.completed_date, t.work_time, "
        "COALESCE(d.work_time, 0) "
        "FROM TASKS t "
        "LEFT JOIN TASK_WORK_TIME_BY_DAY d ON d.task_id = t.id AND d.day = {1} "
        "WHERE {0}", clause, today_num) };
    while (query.executeStep()) {
        auto id = query.getColumn(0).getInt64();
        auto name = query.getColumn(1).getString();
        auto status = query.getColumn(2).getInt();
        auto created_date = query.getColumn(3).getInt64();
        auto started_date = query.getColumn(4).getInt64();
        auto completed_date = query.getColumn(5).getInt64();
        auto work_time = query.getColumn(6).getDouble();
        auto today_work_time = query.getColumn(7).getDouble();

        tasks.emplace_back(id, name, Task::Status { status },
                std::chrono::system_clock::from_time_t(created_date),
                std::chrono::system_clock::from_time_t(started_date),
                std::chrono::system_clock::from_time_t(completed_date),
                hours_to_duration(work_time),
                hours_to_duration(today_work_time)
        );
    }
    return tasks;
}

std::vector<Task> Task::get_tasks(SQLite::Database& db) {
    return get_tasks(db, StatusFilter::All);
}


Task Task::load(SQLite::Database& db, long id) {
    SQLite::Statement query { db,
        "SELECT id, name, status, created_date, started_date, completed_date, work_time "
        "FROM TASKS WHERE id = ?" };
    query.bind(1, static_cast<int64_t>(id));
    if (!query.executeStep()) {
        throw std::runtime_error { std::format("Task {0} not found", id) };
    }
    auto tid = query.getColumn(0).getInt64();
    auto name = query.getColumn(1).getString();
    auto status = query.getColumn(2).getInt();
    auto created_date = query.getColumn(3).getInt64();
    auto started_date = query.getColumn(4).getInt64();
    auto completed_date = query.getColumn(5).getInt64();
    auto work_time = query.getColumn(6).getDouble();

    auto today_work_time = query_today_work_time(db, tid);

    return Task { tid, name, Task::Status { status },
        std::chrono::system_clock::from_time_t(created_date),
        std::chrono::system_clock::from_time_t(started_date),
        std::chrono::system_clock::from_time_t(completed_date),
        hours_to_duration(work_time),
        today_work_time
    };
}

void Task::remove(SQLite::Database& db) {
    if (m_id < 0) return;
    {
        SQLite::Statement daily { db, "DELETE FROM TASK_WORK_TIME_BY_DAY WHERE task_id = ?" };
        daily.bind(1, static_cast<int64_t>(m_id));
        daily.exec();
    }
    SQLite::Statement query { db, "DELETE FROM TASKS WHERE id = ?" };
    query.bind(1, static_cast<int64_t>(m_id));
    query.exec();
    m_id = -1;
}

void Task::create_tables(SQLite::Database& db) {
    SQLite::Statement tasks { db, "CREATE TABLE IF NOT EXISTS TASKS ( \
        id INTEGER PRIMARY KEY AUTOINCREMENT, \
        name TEXT NOT NULL, \
        status INTEGER NOT NULL, \
        created_date INTEGER NOT NULL, \
        started_date INTEGER NOT NULL, \
        completed_date INTEGER NOT NULL, \
        work_time DOUBLE NOT NULL \
    )" };
    tasks.exec();

    // Per-day work time. `day` is the number of days since the local epoch
    // (std::chrono::local_days). Composite primary key keeps upserts cheap.
    SQLite::Statement daily { db, "CREATE TABLE IF NOT EXISTS TASK_WORK_TIME_BY_DAY ( \
        task_id INTEGER NOT NULL, \
        day INTEGER NOT NULL, \
        work_time DOUBLE NOT NULL, \
        PRIMARY KEY (task_id, day), \
        FOREIGN KEY (task_id) REFERENCES TASKS(id) ON DELETE CASCADE \
    )" };
    daily.exec();
}


time_point Task::start_work() noexcept {
    auto now = std::chrono::system_clock::now();
    if (m_status == Status::NotStarted) {
        m_started_date = now;
    }
    m_status = Status::Working;
    return now;
}

void Task::pause_work(time_point start_time) noexcept {
    auto now = std::chrono::system_clock::now();
    m_status = Status::Paused;
    auto elapsed = now - start_time;
    m_work_time += elapsed;
    // Simplification: attribute the whole session to the day it ended on.
    // Sessions crossing midnight are not split across calendar days.
    m_today_work_time += elapsed;
}

void Task::complete_work(time_point start_time) noexcept {
    auto now = std::chrono::system_clock::now();
    m_completed_date = now;
    m_status = Status::Done;
    auto elapsed = now - start_time;
    m_work_time += elapsed;
    m_today_work_time += elapsed;
}
