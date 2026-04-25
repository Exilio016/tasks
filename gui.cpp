#include "gui.hpp"
#include "task.hpp"

#include <SQLiteCpp/Database.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <format>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace gui {

namespace {

struct AppState {
    SQLite::Database *db = nullptr;
    GtkWindow *window = nullptr;
    GtkListBox *list = nullptr;
    GtkEntry *entry = nullptr;
    // Filter UI: a plain button that owns a popover we create/destroy on
    // every open. This gives us total control over dismissal (GtkDropDown's
    // built-in popover wouldn't close reliably after a selection).
    GtkButton *filter_button = nullptr;
    GtkPopover *filter_popover = nullptr;
    Task::StatusFilter filter = Task::StatusFilter::Pending;
    // Tracks active "start" timestamps for tasks currently in Working state,
    // keyed by task id. Needed because Task::pause_work / complete_work
    // require the start time returned by Task::start_work.
    std::unordered_map<long, std::chrono::system_clock::time_point> running;
};

// Per-row live data used by the 1s tick to update timer labels in place
// without rebuilding the list. Owned by the row via g_object_set_data_full.
struct TaskRowData {
    long id;
    Task::Status status;
    // Snapshot of the totals persisted at the moment the row was built.
    // Live values = base + (now - st->running[id]) while Working.
    std::chrono::system_clock::duration base_work;
    std::chrono::system_clock::duration base_today;
    GtkLabel *meta_label;
    GtkLabel *today_label;
};

struct FilterOption {
    const char *label;
    Task::StatusFilter value;
};

constexpr FilterOption kFilterOptions[] = {
    {"All",         Task::StatusFilter::All},
    {"Pending",     Task::StatusFilter::Pending},
    {"Working",     Task::StatusFilter::Working},
    {"Paused",      Task::StatusFilter::Paused},
    {"Not started", Task::StatusFilter::NotStarted},
    {"Done",        Task::StatusFilter::Done},
};

constexpr const char *filter_label(Task::StatusFilter f) {
    for (const auto &o : kFilterOptions) {
        if (o.value == f) return o.label;
    }
    return "Filter";
}

constexpr const char *status_label(Task::Status s) {
    switch (s) {
        case Task::Status::NotStarted: return "Not started";
        case Task::Status::Working:    return "Working";
        case Task::Status::Paused:     return "Paused";
        case Task::Status::Done:       return "Done";
    }
    return "?";
}

std::string format_duration(std::chrono::system_clock::duration d) {
    using namespace std::chrono;
    auto secs = duration_cast<seconds>(d).count();
    auto h = secs / 3600;
    auto m = (secs % 3600) / 60;
    auto s = secs % 60;
    return std::format("{:02}:{:02}:{:02}", h, m, s);
}

// Format a time_point as a local YYYY-MM-DD HH:MM string. Unset dates
// (epoch) are rendered as an em-dash so the detail row stays tidy for
// tasks that haven't been started/completed yet.
std::string format_datetime(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    if (t <= 0) return "—";
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

void refresh_list(AppState *st);

constexpr const char *kTaskStatusKey = "task-status";

// Encodes Task::Status on a row. We store (status + 1) so that a missing
// value (returned as nullptr/0) can be distinguished from Status::NotStarted.
void attach_row_status(GtkWidget *row, Task::Status s) {
    g_object_set_data(G_OBJECT(row), kTaskStatusKey,
                      reinterpret_cast<gpointer>(
                          static_cast<intptr_t>(s) + 1));
}

gboolean list_filter_func(GtkListBoxRow *row, gpointer data) {
    auto *st = static_cast<AppState *>(data);
    auto raw = reinterpret_cast<intptr_t>(
        g_object_get_data(G_OBJECT(row), kTaskStatusKey));
    if (raw == 0) return TRUE;
    auto status = static_cast<Task::Status>(raw - 1);

    switch (st->filter) {
        case Task::StatusFilter::All:
            return TRUE;
        case Task::StatusFilter::NotStarted:
            return status == Task::Status::NotStarted;
        case Task::StatusFilter::Working:
            return status == Task::Status::Working;
        case Task::StatusFilter::Paused:
            return status == Task::Status::Paused;
        case Task::StatusFilter::Done:
            return status == Task::Status::Done;
        case Task::StatusFilter::Pending:
            return status == Task::Status::NotStarted
                || status == Task::Status::Working
                || status == Task::Status::Paused;
    }
    return TRUE;
}

long button_task_id(GtkWidget *btn) {
    return static_cast<long>(reinterpret_cast<intptr_t>(
        g_object_get_data(G_OBJECT(btn), "task-id")));
}

void attach_task_id(GtkWidget *btn, long id) {
    g_object_set_data(G_OBJECT(btn), "task-id",
                      reinterpret_cast<gpointer>(static_cast<intptr_t>(id)));
}

void on_start_clicked(GtkButton *btn, gpointer data) {
    auto *st = static_cast<AppState *>(data);
    long id = button_task_id(GTK_WIDGET(btn));
    try {
        Task t = Task::load(*st->db, id);
        auto start = t.start_work();
        st->running[id] = start;
        t.save(*st->db);
    } catch (const std::exception &e) {
        g_warning("Failed to start task %ld: %s", id, e.what());
    }
    refresh_list(st);
}

void on_pause_clicked(GtkButton *btn, gpointer data) {
    auto *st = static_cast<AppState *>(data);
    long id = button_task_id(GTK_WIDGET(btn));
    try {
        Task t = Task::load(*st->db, id);
        auto it = st->running.find(id);
        auto start = (it != st->running.end())
                         ? it->second
                         : std::chrono::system_clock::now();
        t.pause_work(start);
        st->running.erase(id);
        t.save(*st->db);
    } catch (const std::exception &e) {
        g_warning("Failed to pause task %ld: %s", id, e.what());
    }
    refresh_list(st);
}

void on_remove_clicked(GtkButton *btn, gpointer data) {
    auto *st = static_cast<AppState *>(data);
    long id = button_task_id(GTK_WIDGET(btn));
    try {
        Task t = Task::load(*st->db, id);
        t.remove(*st->db);
        st->running.erase(id);
    } catch (const std::exception &e) {
        g_warning("Failed to remove task %ld: %s", id, e.what());
    }
    refresh_list(st);
}

void on_done_clicked(GtkButton *btn, gpointer data) {
    auto *st = static_cast<AppState *>(data);
    long id = button_task_id(GTK_WIDGET(btn));
    try {
        Task t = Task::load(*st->db, id);
        // If task was never started, start it so m_started_date is set
        // and work_time is tracked correctly.
        std::chrono::system_clock::time_point start;
        auto it = st->running.find(id);
        if (it != st->running.end()) {
            start = it->second;
        }  else {
            start = std::chrono::system_clock::now();
        }
        t.complete_work(start);
        st->running.erase(id);
        t.save(*st->db);
    } catch (const std::exception &e) {
        g_warning("Failed to complete task %ld: %s", id, e.what());
    }
    refresh_list(st);
}

GtkWidget *make_action_button(const char *label, const char *css_class,
                              long task_id, GCallback cb, AppState *st) {
    GtkWidget *b = gtk_button_new_with_label(label);
    if (css_class) gtk_widget_add_css_class(b, css_class);
    gtk_widget_add_css_class(b, "task-action");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    attach_task_id(b, task_id);
    g_signal_connect(b, "clicked", cb, st);
    return b;
}

// Toggle the revealer associated with the expand arrow, and flip the arrow
// icon between "collapsed" and "expanded" states. The revealer is attached
// to the button via g_object_set_data so rows stay self-contained.
void on_expand_clicked(GtkButton *btn, gpointer) {
    auto *revealer = GTK_REVEALER(
        g_object_get_data(G_OBJECT(btn), "task-revealer"));
    if (!revealer) return;
    gboolean expanded = !gtk_revealer_get_reveal_child(revealer);
    gtk_revealer_set_reveal_child(revealer, expanded);
    gtk_button_set_icon_name(btn,
        expanded ? "pan-down-symbolic" : "pan-end-symbolic");
}

GtkWidget *make_row(AppState *st, const Task &task) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "task-row");

    // Outer vertical container: header row on top, expandable details below.
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(hbox, 12);
    gtk_widget_set_margin_end(hbox, 12);
    gtk_widget_set_margin_top(hbox, 8);
    gtk_widget_set_margin_bottom(hbox, 8);

    // Collapsed-by-default expand toggle. Icon swaps in on_expand_clicked.
    GtkWidget *expand_btn = gtk_button_new_from_icon_name("pan-end-symbolic");
    gtk_widget_add_css_class(expand_btn, "flat");
    gtk_widget_add_css_class(expand_btn, "task-expand");
    gtk_widget_set_valign(expand_btn, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), expand_btn);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);

    GtkWidget *name = gtk_label_new(std::string(task.get_name()).c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_widget_add_css_class(name, "task-name");
    gtk_widget_add_css_class(name, "title-4");

    auto meta = std::format("{}  •  {}",
                            status_label(task.get_status()),
                            format_duration(task.get_work_time()));
    GtkWidget *sub = gtk_label_new(meta.c_str());
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_widget_add_css_class(sub, "dim-label");
    gtk_widget_add_css_class(sub, "caption");

    gtk_box_append(GTK_BOX(vbox), name);
    gtk_box_append(GTK_BOX(vbox), sub);
    gtk_box_append(GTK_BOX(hbox), vbox);

    // Action buttons
    long id = task.get_id();
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);

    switch (task.get_status()) {
        case Task::Status::NotStarted: {
            gtk_box_append(GTK_BOX(actions),
                make_action_button("Start", "suggested-action", id,
                                   G_CALLBACK(on_start_clicked), st));
            gtk_box_append(GTK_BOX(actions),
                make_action_button("Done", nullptr, id,
                                   G_CALLBACK(on_done_clicked), st));
            break;
        }
        case Task::Status::Working: {
            gtk_box_append(GTK_BOX(actions),
                make_action_button("Pause", nullptr, id,
                                   G_CALLBACK(on_pause_clicked), st));
            gtk_box_append(GTK_BOX(actions),
                make_action_button("Done", "suggested-action", id,
                                   G_CALLBACK(on_done_clicked), st));
            break;
        }
        case Task::Status::Paused: {
            gtk_box_append(GTK_BOX(actions),
                make_action_button("Resume", "suggested-action", id,
                                   G_CALLBACK(on_start_clicked), st));
            gtk_box_append(GTK_BOX(actions),
                make_action_button("Done", nullptr, id,
                                   G_CALLBACK(on_done_clicked), st));
            break;
        }
        case Task::Status::Done: {
            gtk_box_append(GTK_BOX(actions),
                make_action_button("Remove", "destructive-action", id,
                                   G_CALLBACK(on_remove_clicked), st));
            break;
        }
    }

    gtk_box_append(GTK_BOX(hbox), actions);
    gtk_box_append(GTK_BOX(outer), hbox);

    // Expandable details: created / started / completed timestamps.
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(details, "task-details");
    gtk_widget_set_margin_start(details, 44);
    gtk_widget_set_margin_end(details, 12);
    gtk_widget_set_margin_bottom(details, 10);

    struct DetailRow {
        const char *label;
        std::string value;
    };
    const DetailRow rows[] = {
        {"Created",   format_datetime(task.get_created_date())},
        {"Started",   format_datetime(task.get_started_date())},
        {"Completed", format_datetime(task.get_completed_date())},
    };
    for (const auto &r : rows) {
        GtkWidget *line = gtk_label_new(
            std::format("{}: {}", r.label, r.value).c_str());
        gtk_label_set_xalign(GTK_LABEL(line), 0.0f);
        gtk_widget_add_css_class(line, "caption");
        gtk_widget_add_css_class(line, "dim-label");
        gtk_box_append(GTK_BOX(details), line);
    }

    // Kept as a separate label (not part of the static loop above) so the
    // per-second tick can update it in place while the task is Working.
    GtkWidget *today_line = gtk_label_new(
        std::format("Today: {}",
                    format_duration(task.get_today_work_time())).c_str());
    gtk_label_set_xalign(GTK_LABEL(today_line), 0.0f);
    gtk_widget_add_css_class(today_line, "caption");
    gtk_widget_add_css_class(today_line, "dim-label");
    gtk_box_append(GTK_BOX(details), today_line);

    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    gtk_revealer_set_child(GTK_REVEALER(revealer), details);
    gtk_box_append(GTK_BOX(outer), revealer);

    g_object_set_data(G_OBJECT(expand_btn), "task-revealer", revealer);
    g_signal_connect(expand_btn, "clicked",
                     G_CALLBACK(on_expand_clicked), nullptr);

    // Attach live-update state. Destroyed automatically when the row is
    // unparented (on refresh_list or window close), so there's no leak.
    auto *rd = new TaskRowData{
        id,
        task.get_status(),
        task.get_work_time(),
        task.get_today_work_time(),
        GTK_LABEL(sub),
        GTK_LABEL(today_line),
    };
    g_object_set_data_full(
        G_OBJECT(row), "task-row-data", rd,
        [](gpointer p) { delete static_cast<TaskRowData *>(p); });

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
    return row;
}

void on_add_clicked(GtkButton *, gpointer data) {
    auto *st = static_cast<AppState *>(data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(st->entry));
    if (!text || !*text) return;

    try {
        Task t{text};
        t.save(*st->db);
    } catch (const std::exception &e) {
        g_warning("Failed to save task: %s", e.what());
    }

    gtk_editable_set_text(GTK_EDITABLE(st->entry), "");
    refresh_list(st);
}

void on_entry_activate(GtkEntry *, gpointer data) {
    on_add_clicked(nullptr, data);
}

// Tear down the current filter popover, if any. Called both when the user
// picks an option and when re-opening the menu (we always recreate).
void destroy_filter_popover(AppState *st) {
    if (!st->filter_popover) return;
    GtkWidget *w = GTK_WIDGET(st->filter_popover);
    gtk_popover_popdown(st->filter_popover);
    gtk_widget_unparent(w);
    st->filter_popover = nullptr;
}

void on_filter_option_clicked(GtkButton *btn, gpointer data) {
    auto *st = static_cast<AppState *>(data);
    auto value = static_cast<Task::StatusFilter>(
        reinterpret_cast<intptr_t>(
            g_object_get_data(G_OBJECT(btn), "filter-value")));

    st->filter = value;
    gtk_button_set_label(st->filter_button, filter_label(value));

    // Remove the popover from the UI, then shift focus to the main UI so
    // nothing in the (now destroyed) menu retains focus.
    destroy_filter_popover(st);
    gtk_list_box_invalidate_filter(st->list);
    if (st->entry) {
        gtk_widget_grab_focus(GTK_WIDGET(st->entry));
    } else if (st->window) {
        gtk_widget_grab_focus(GTK_WIDGET(st->window));
    }
}

void on_filter_button_clicked(GtkButton *btn, gpointer data) {
    auto *st = static_cast<AppState *>(data);

    // Always recreate the menu so there's no stale popover lingering in
    // the widget tree.
    destroy_filter_popover(st);

    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_autohide(GTK_POPOVER(popover), TRUE);
    gtk_widget_set_parent(popover, GTK_WIDGET(btn));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "filter-menu");
    gtk_popover_set_child(GTK_POPOVER(popover), box);

    for (const auto &o : kFilterOptions) {
        GtkWidget *opt = gtk_button_new_with_label(o.label);
        gtk_widget_add_css_class(opt, "flat");
        gtk_widget_set_halign(opt, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(opt, TRUE);
        g_object_set_data(G_OBJECT(opt), "filter-value",
                          reinterpret_cast<gpointer>(
                              static_cast<intptr_t>(o.value)));
        g_signal_connect(opt, "clicked",
                         G_CALLBACK(on_filter_option_clicked), st);
        gtk_box_append(GTK_BOX(box), opt);
    }

    st->filter_popover = GTK_POPOVER(popover);
    gtk_popover_popup(GTK_POPOVER(popover));
    gtk_widget_grab_focus(popover);
}

// Fires every second from the GLib main loop. Updates the visible meta and
// "Today" labels for rows whose task is currently Working, without touching
// the database or rebuilding the list. Returns G_SOURCE_CONTINUE so the
// source stays attached for the app's lifetime.
gboolean on_tick(gpointer data) {
    auto *st = static_cast<AppState *>(data);
    if (!st || !st->list) return G_SOURCE_CONTINUE;

    auto now = std::chrono::system_clock::now();
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(st->list));
         child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        auto *rd = static_cast<TaskRowData *>(
            g_object_get_data(G_OBJECT(child), "task-row-data"));
        if (!rd || rd->status != Task::Status::Working) continue;

        auto it = st->running.find(rd->id);
        if (it == st->running.end()) continue;

        auto elapsed = now - it->second;
        auto total = rd->base_work + elapsed;
        auto today = rd->base_today + elapsed;

        if (rd->meta_label) {
            gtk_label_set_text(
                rd->meta_label,
                std::format("{}  •  {}",
                            status_label(rd->status),
                            format_duration(total)).c_str());
        }
        if (rd->today_label) {
            gtk_label_set_text(
                rd->today_label,
                std::format("Today: {}", format_duration(today)).c_str());
        }
    }
    return G_SOURCE_CONTINUE;
}

void refresh_list(AppState *st) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(st->list));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(st->list, child);
        child = next;
    }

    std::vector<Task> tasks;
    try {
        // Always load all tasks; the GtkListBox filter function decides
        // which rows are visible for the current dropdown selection.
        tasks = Task::get_tasks(*st->db, Task::StatusFilter::All);
    } catch (const std::exception &e) {
        g_warning("Failed to load tasks: %s", e.what());
    }

    for (const auto &t : tasks) {
        GtkWidget *row = make_row(st, t);
        attach_row_status(row, t.get_status());
        gtk_list_box_append(st->list, row);
    }
}

void load_css() {
    const char *css = R"CSS(
        window { background-color: #1e1e22; }
        .task-row { border-radius: 8px; margin: 2px 8px; padding: 2px; }
        .task-row:hover { background-color: alpha(white, 0.05); }
        .task-name { font-weight: 600; }
        entry { padding: 8px 12px; border-radius: 8px; }
        button.suggested-action { border-radius: 8px; }
        .filter-menu { padding: 4px; min-width: 140px; }
        .filter-menu button { padding: 6px 10px; border-radius: 6px; }
        .task-expand { min-width: 20px; min-height: 20px; padding: 2px; }
        .task-details { opacity: 0.9; }
    )CSS";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void on_activate(GtkApplication *app, gpointer user_data) {
    auto *st = static_cast<AppState *>(user_data);

    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, nullptr);
    // Some environments (notably WSLg) default the header-bar decoration
    // layout to just ":close". Pin it so minimize/maximize/close are all
    // available on the right side of the titlebar.
    g_object_set(settings, "gtk-decoration-layout",
                 ":minimize,maximize,close", nullptr);

    load_css();

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Tasks");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 640);
    // Belt-and-braces: window must be resizable/deletable for min/max/close
    // buttons to be meaningful, and we want them actually shown.
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    gtk_window_set_deletable(GTK_WINDOW(window), TRUE);
    st->window = GTK_WINDOW(window);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    // Custom filter control: a plain button whose popover we build on demand.
    // Default filter is "Pending".
    st->filter = Task::StatusFilter::Pending;
    GtkWidget *filter_btn = gtk_button_new_with_label(filter_label(st->filter));
    gtk_widget_add_css_class(filter_btn, "filter-button");
    st->filter_button = GTK_BUTTON(filter_btn);
    g_signal_connect(filter_btn, "clicked",
                     G_CALLBACK(on_filter_button_clicked), st);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), filter_btn);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);

    GtkWidget *input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "New task name...");
    gtk_widget_set_hexpand(entry, TRUE);
    st->entry = GTK_ENTRY(entry);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), st);

    GtkWidget *add_btn = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add_btn, "suggested-action");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), st);

    gtk_box_append(GTK_BOX(input_row), entry);
    gtk_box_append(GTK_BOX(input_row), add_btn);
    gtk_box_append(GTK_BOX(root), input_row);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    st->list = GTK_LIST_BOX(list);

    // Shown automatically when the list has no visible rows (either truly
    // empty or hidden by the current filter).
    GtkWidget *placeholder = gtk_label_new("No tasks match the current filter.");
    gtk_widget_add_css_class(placeholder, "dim-label");
    gtk_widget_set_margin_top(placeholder, 24);
    gtk_widget_set_margin_bottom(placeholder, 24);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(list), placeholder);

    gtk_list_box_set_filter_func(GTK_LIST_BOX(list), list_filter_func, st, nullptr);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
    gtk_box_append(GTK_BOX(root), scroller);

    gtk_window_set_child(GTK_WINDOW(window), root);

    refresh_list(st);

    // 1Hz tick to animate timers for Working tasks. Lives for the lifetime
    // of the process; no removal needed since we never recreate the window.
    g_timeout_add_seconds(1, on_tick, st);

    gtk_window_present(GTK_WINDOW(window));
}

// Pause every task that's currently in the Working state and persist it.
// Used on graceful shutdown (window close, Ctrl+C, SIGTERM) so elapsed work
// time isn't lost and no task stays "Working" across runs.
void pause_all_running(AppState *st) {
    if (!st || !st->db) return;
    for (auto &[id, start] : st->running) {
        try {
            Task t = Task::load(*st->db, id);
            if (t.get_status() == Task::Status::Working) {
                t.pause_work(start);
                t.save(*st->db);
            }
        } catch (const std::exception &e) {
            g_warning("Failed to pause task %ld on shutdown: %s", id, e.what());
        }
    }
    st->running.clear();
}

void on_app_shutdown(GApplication *, gpointer data) {
    pause_all_running(static_cast<AppState *>(data));
}

// Posix signal handler installed via g_unix_signal_add. Runs on the GLib
// main loop thread, so it's safe to touch GtkApplication / AppState from
// here. Returning G_SOURCE_REMOVE detaches the source after first delivery.
gboolean on_termination_signal(gpointer data) {
    auto *app = G_APPLICATION(data);
    g_application_quit(app);  // triggers "shutdown" -> on_app_shutdown
    return G_SOURCE_REMOVE;
}

}  // namespace

int run(int argc, char **argv, SQLite::Database &db) {
    AppState st;
    st.db = &db;

    // Keep the app id, prgname, and WM_CLASS in sync so WSLg advertises a
    // stable identity. This must match the System.AppUserModel.ID stamped
    // on the Windows shortcut for taskbar grouping to work.
    constexpr const char *kAppId = "com.tasks.app";
    g_set_prgname(kAppId);
    g_set_application_name("Tasks");

    GtkApplication *app = gtk_application_new(kAppId,
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &st);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), &st);

    // Catch Ctrl+C and SIGTERM so tasks get paused/saved before exit.
    g_unix_signal_add(SIGINT, on_termination_signal, app);
    g_unix_signal_add(SIGTERM, on_termination_signal, app);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

}  // namespace gui
