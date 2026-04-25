#ifndef __TASK_HPP_
#define __TASK_HPP_
#include <string>
#include <chrono>
#include <SQLiteCpp/Database.h>
#include <map>
#include <vector>

class Task {
    using time_point = std::chrono::system_clock::time_point;
    using duration = std::chrono::system_clock::duration;

    public:
        using date = std::chrono::year_month_day;

        enum struct Status {
            NotStarted,
            Working,
            Paused,
            Done
        };

        explicit Task(std::string_view name) noexcept;
        explicit Task(long id, std::string_view name, Status status, time_point create_date, time_point started_date, time_point completed_date, duration work_time, duration today_work_time = duration::zero()) noexcept;

        constexpr long get_id() const noexcept { return m_id; }
        constexpr std::string_view get_name() const noexcept { return m_name; }
        constexpr Status get_status() const noexcept { return m_status; }
        constexpr duration get_work_time() const noexcept { return m_work_time; }
        constexpr duration get_today_work_time() const noexcept { return m_today_work_time; }
        constexpr time_point get_created_date() const noexcept { return m_created_date; }
        constexpr time_point get_started_date() const noexcept { return m_started_date; }
        constexpr time_point get_completed_date() const noexcept { return m_completed_date; }

        // Lazily retrieve per-day work time from the database. Returns an
        // ordered map keyed by calendar day. Not cached on the instance.
        std::map<date, duration> get_work_time_by_day(SQLite::Database& db) const;

        time_point start_work() noexcept;
        void pause_work(time_point start_time) noexcept;
        void complete_work(time_point start_time) noexcept;

        void save(SQLite::Database& db);
        void remove(SQLite::Database& db);
        static Task load(SQLite::Database& db, long id);

        enum class StatusFilter {
            All,
            Pending,
            Done,
            Paused,
            Working,
            NotStarted
        };
        static std::vector<Task> get_tasks(SQLite::Database& db);
        static std::vector<Task> get_tasks(SQLite::Database& db, StatusFilter filter);
        static void create_tables(SQLite::Database& db);
    private:

        long m_id;
        std::string m_name;
        Status m_status;
        time_point m_created_date;
        time_point m_started_date;
        time_point m_completed_date;
        duration m_work_time;
        // Accumulated work time for the current calendar day. Populated on
        // load from TASK_WORK_TIME_BY_DAY and written back on save().
        duration m_today_work_time;
};
#endif
