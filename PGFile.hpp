#pragma once

#ifdef WITH_POSTGRESQL

#include "apostol/bot_session.hpp"
#include "apostol/fetch_client.hpp"
#include "apostol/module.hpp"
#include "apostol/pg.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace apostol
{

class Application;
class EventLoop;

// ─── PGFile ──────────────────────────────────────────────────────────────────
//
// Helper module that listens for PG NOTIFY on channel "file" and synchronizes
// files between db.file and the filesystem.
//
// Mirrors v1 CPGFile from debt-master.
//
// Lifecycle:
//   on_start()  → pool_.listen("file", ...)
//   on_notify() → parse payload JSON, enqueue FileTask
//   heartbeat() → bot_.refresh_if_needed() + process_queue() + check_timeouts()
//   on_stop()   → pool_.unlisten("file"), bot_.sign_out()
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
        std::chrono::steady_clock::time_point deadline;
    };

    void on_notify(std::string_view channel, std::string_view payload);
    void do_file(std::shared_ptr<FileTask> task);
    void do_curl(std::shared_ptr<FileTask> task, const std::string& url);
    void do_done(std::shared_ptr<FileTask> task,
                 std::string_view body, std::string_view content_type);
    void do_fail(std::shared_ptr<FileTask> task, std::string_view message);
    void process_queue();
    void check_timeouts(std::chrono::steady_clock::time_point now);
    void remove_task(const std::string& id);

    PgPool&               pool_;
    FetchClient           fetch_;
    BotSession            bot_;
    std::filesystem::path files_path_;
    int                   timeout_secs_;
    bool                  enabled_;

    std::deque<std::shared_ptr<FileTask>> queue_;
};

} // namespace apostol

#endif // WITH_POSTGRESQL
