#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/bot_session.hpp"
#include "apostol/fetch_client.hpp"
#include "apostol/module.hpp"
#include "apostol/pg.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Path `single` of the notify channel `file` needs the db-platform subscriber;
// a libapostol without it builds the module with the path `legacy` only.
#if defined(WITH_DB_PLATFORM) && __has_include("apostol/notify_subscriber.hpp")
#include "apostol/notify_subscriber.hpp"
#define PGFILE_NOTIFY 1
#endif

namespace apostol
{

class Application;
class EventLoop;
class Logger;
class ModuleManager;

// ─── PGFile ──────────────────────────────────────────────────────────────────
//
// Helper module that synchronizes files between db.file and the filesystem,
// on the events of the notify channel "file".
//
// Mirrors v1 CPGFile from debt-master.
//
// Two paths, by the mode of the notify channel `file` (notify.send decides
// which one an event takes; both may be live during a switch):
//
// Path `legacy` (and `broadcast`) — a NOTIFY on "file" carries the event,
// every listener hears it:
//   on_start()  → probe for notify, pool_.listen("file", ...)
//   on_notify() → parse payload JSON, enqueue FileTask
//   heartbeat() → bot_.refresh_if_needed() + process_queue() + check_timeouts()
//   on_stop()   → pool_.unlisten("file"), bot_.sign_out()
//
// Path `single` — a row of notify.queue, handed to one PGFile; the
// volume is shared, so one write per event is the whole of it:
//   heartbeat() → until the channel is read `single`: notify.channel_policy;
//                 then a NotifySubscriber of `file` for the life of the module
//                 (rows are brought to their end in any mode)
//   a row       → the same payload as the NOTIFY, the same task
//   an outcome  → the done/fail callback and notify.ack in one transaction;
//                 a row not worked on (queue full, stopping, not started in
//                 time) is given back with notify.nack "unsent: …"
//
// Operations by type:
//   DELETE        → delete file from disk
//   INSERT/UPDATE:
//     type "-"    → base64 decode data from PG → write to disk
//     type "l"    → base64 decode URL → curl fetch → write to disk
//     type "s"    → delete local file (S3 is authoritative)
//
class PGFile final : public Module
{
public:
    PGFile(Application& app, EventLoop& loop);

    std::string_view name() const override { return "PGFile"; }
    bool enabled() const override { return enabled_; }

    // PGFile does not handle incoming HTTP requests — it's a helper.
    bool execute(const HttpRequest&, HttpResponse&) override { return false; }

    void on_start() override;
    void on_stop() override;
    void heartbeat(std::chrono::system_clock::time_point now) override;
#ifdef PGFILE_NOTIFY
    void on_stopping() override;
    bool busy() const override;
#endif

private:
    struct FileTask
    {
        std::string id;           // file UUID
        std::string session;      // from NOTIFY payload
        std::string operation;    // INSERT, UPDATE, DELETE
        std::string type;         // "-", "l", "s", "d"
        std::string path;
        std::string name;
        std::string hash;
        std::string done;         // PG callback function
        std::string fail;         // PG callback function
        std::string absolute_name;
        bool in_progress{false};
        // The outcome is on its way (or none is due): a late one — a download
        // that ends after the timeout failed the task — is not written beside it.
        bool settling{false};
        std::chrono::steady_clock::time_point deadline;
#ifdef PGFILE_NOTIFY
        // Path `single`: the row of notify.queue the event came with; the
        // outcome goes out beside notify.ack under this token. A delivery of
        // the same row while the task is still here replaces it.
        std::optional<db_platform::NotifyRow> row;
#endif
    };

    // The statement written instead when the first one failed; empty — none.
    using Fallback = std::function<std::string(std::string_view error)>;

    void on_notify(std::string_view channel, std::string_view payload);
    // nullptr — the event is not one to act on (FR.02); @p why says what.
    std::shared_ptr<FileTask> make_task(std::string_view payload, std::string& why) const;
    void do_file(std::shared_ptr<FileTask> task);
    void do_curl(std::shared_ptr<FileTask> task, const std::string& url);
    void do_done(std::shared_ptr<FileTask> task,
                 std::string_view body, std::string_view content_type);
    void do_fail(std::shared_ptr<FileTask> task, std::string_view message);
    // The end of a task: @p sql (may be empty) and, on the path single, the
    // confirmation of its row with it.
    void finish(const std::shared_ptr<FileTask>& task, std::string sql, Fallback fallback = {});
    void process_queue();
    void check_timeouts(std::chrono::steady_clock::time_point now);
    void remove_task(const std::shared_ptr<FileTask>& task);

    void probe();
    void read_mode();

#ifdef PGFILE_NOTIFY
    void start_notify();
    void on_row(const db_platform::NotifyRow& row);
    void settle_single(std::shared_ptr<FileTask> task, std::string sql, Fallback fallback);
    void settle_again(std::shared_ptr<FileTask> task, db_platform::NotifyRow row, std::string sql,
                      Fallback fallback, bool last);
    void settle_stale(std::shared_ptr<FileTask> task, const db_platform::NotifyRow& row,
                      std::string sql, Fallback fallback);
    void give_back(const std::shared_ptr<FileTask>& task, std::string_view reason);
    void stop_notify();
#endif

    PgPool&               pool_;
    Logger&               log_;
    ModuleManager&        modules_;
    FetchClient           fetch_;
    BotSession            bot_;
    std::filesystem::path files_path_;
    int                   timeout_secs_;
    bool                  enabled_;
    // The helper runs under the leader gate (process.leader): one PGFile per
    // database. Without it, every copy acts on a broadcast event.
    bool                  gated_;

    // The database has notify (notify.take): the mode of `file` is read.
    // Unknown until the probe answers.
    std::optional<bool>   has_notify_;
    bool                  probing_{false};
    bool                  reading_mode_{false};
    bool                  said_ungated_{false};
    long                  refused_lease_{-1};   // the lease last refused (read_mode), said once
    std::chrono::steady_clock::time_point next_probe_{};
    std::chrono::steady_clock::time_point next_mode_{};

#ifdef PGFILE_NOTIFY
    EventLoop&            loop_;
    std::string           node_;
    std::unique_ptr<db_platform::NotifySubscriber> notify_;
#endif

    std::deque<std::shared_ptr<FileTask>> queue_;
    std::size_t max_queue_size_{1000};
};

} // namespace apostol

#endif // WITH_POSTGRESQL
