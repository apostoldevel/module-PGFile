#ifdef WITH_POSTGRESQL

#include "PGFile.hpp"
#include "apostol/application.hpp"

#include "apostol/base64.hpp"
#include "apostol/file_utils.hpp"
#include "apostol/pg_utils.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <cstdlib>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol
{

namespace
{

// A task is bounded by timeout + 10 s twice: waiting to start (a row not
// started by then is given back) and running (failed by then). The
// confirmation that follows needs its own time — the margin PGFetch keeps.
constexpr long deadline_extra_s = 10;
constexpr long confirm_margin_s = 20;

constexpr auto probe_retry = std::chrono::seconds(5);
// No notify in the database: asked again, the platform may be updated under a running backend.
constexpr auto probe_again = std::chrono::seconds(60);
constexpr auto mode_every  = std::chrono::seconds(10);

// The helper's leader gate (libapostol: process.leader). A libapostol without
// it has no gate: every copy runs.
template <typename Settings>
bool leader_gate(const Settings& s)
{
    if constexpr (requires { s.leader; })
        return s.leader;
    else
        return false;
}

// The limit on a fetched file's size (libapostol: set_max_response_size). A
// libapostol without it has no limit to set: false.
template <typename Client>
bool set_size_limit(Client& c, std::size_t bytes)
{
    if constexpr (requires { c.set_max_response_size(bytes); }) {
        c.set_max_response_size(bytes);
        return true;
    } else {
        return false;
    }
}

// module.PGFile.max_file_size: a whole number of bytes up to 2^63 - 1 (the
// largest limit libcurl takes), 0 — no limit; written as a string when it
// comes from the environment ("${VAR}"), an empty or blank one is an absent
// key. Anything else: nullopt and `bad` set.
std::optional<std::size_t> read_size(const nlohmann::json& v, bool& bad)
{
    constexpr unsigned long long top = static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    const auto fits = [&](unsigned long long n) -> std::optional<std::size_t> {
        if (n > top || n > std::numeric_limits<std::size_t>::max()) {
            bad = true;
            return std::nullopt;
        }
        return static_cast<std::size_t>(n);
    };

    bad = false;
    if (v.is_number_unsigned())
        return fits(v.get<unsigned long long>());
    if (v.is_number_integer() && v.get<long long>() >= 0)
        return fits(static_cast<unsigned long long>(v.get<long long>()));
    if (v.is_string()) {
        const auto& s     = v.get_ref<const std::string&>();
        const auto  space = [](unsigned char ch) { return std::isspace(ch) != 0; };
        const auto  b     = std::find_if_not(s.begin(), s.end(), space);
        if (b == s.end())
            return std::nullopt;
        const auto e = std::find_if_not(s.rbegin(), s.rend(), space).base();
        unsigned long long n = 0;
        const auto [end, ec] = std::from_chars(&*b, &*b + (e - b), n);
        if (ec == std::errc() && end == &*b + (e - b))
            return fits(n);
    }
    bad = true;
    return std::nullopt;
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

PGFile::PGFile(Application& app, EventLoop& loop)
    : pool_(app.db_pool())
    , log_(app.logger())
    , modules_(app.module_manager())
    , fetch_(loop)
    , bot_(app.db_pool(), "PGFile/2.0", "127.0.0.1")
    , enabled_(true)
    , gated_(leader_gate(app.settings()))
#ifdef PGFILE_NOTIFY
    , loop_(loop)
    , node_(app.node_id())
#endif
{
    // Read path, timeout and the file size limit from config
    std::string path_str;
    int timeout = 60;
    std::optional<std::size_t> max_size;
    if (auto* cfg = app.module_config("PGFile")) {
        if (cfg->contains("path"))
            path_str = (*cfg)["path"].get<std::string>();
        if (cfg->contains("timeout") && (*cfg)["timeout"].is_number())
            timeout = (*cfg)["timeout"].get<int>();
        if (cfg->contains("max_file_size")) {
            bool bad = false;
            max_size = read_size((*cfg)["max_file_size"], bad);
            if (bad)
                log_.warn("PGFile: max_file_size {} is not a whole number of bytes, the default limit stays",
                          (*cfg)["max_file_size"].dump());
        }
    }

    if (max_size && !set_size_limit(fetch_, *max_size))
        log_.warn("PGFile: max_file_size ignored, this build has no limit on a fetched file");

    files_path_ = app.resolve_path(path_str, "files");
    timeout_secs_ = timeout > 0 ? timeout : 60;

    if (timeout_secs_ > 0)
        fetch_.set_timeout(static_cast<long>(timeout_secs_) * 1000);

    // Load OAuth2 "service" credentials for bot session
    auto [client_id, client_secret] = app.providers().credentials("service");
    if (!client_id.empty())
        bot_.set_credentials(client_id, client_secret);
}

// ─── Lifecycle ──────────────────────────────────────────────────────────────

void PGFile::on_start()
{
    probe();

    pool_.listen("file", [this](std::string_view ch, std::string_view payload) {
        try {
            on_notify(ch, payload);
        } catch (const std::exception& e) {
            // Never let exceptions escape from NOTIFY callback
            log_.error("PGFile: event of file not handled: {}", e.what());
        }
    });
}

void PGFile::on_stop()
{
    pool_.unlisten("file");
#ifdef PGFILE_NOTIFY
    stop_notify();
#endif
    bot_.sign_out();
}

// ─── probe / read_mode ──────────────────────────────────────────────────────
//
// The mode of the channel `file` decides the path: `single` — a row of
// notify.queue for one PGFile (the shared common channel is silent then),
// anything else — the NOTIFY on "file" every listener hears. A database
// without notify has the second only.
//
// The pool may refuse a query (an epoll registration that fails): the flag is lowered and the next
// heartbeat sends it again.

void PGFile::probe()
{
    probing_ = true;
    try {
        pool_.execute(
            "SELECT to_regprocedure('notify.take(bigint,integer)') IS NOT NULL"
            " AND to_regprocedure('notify.channel_policy(text)') IS NOT NULL",
            [this](std::vector<PgResult> results) {
                probing_ = false;
                const bool yes = !results.empty() && results[0].rows() > 0 && results[0].columns() > 0 &&
                                 results[0].value(0, 0) != nullptr && results[0].value(0, 0)[0] == 't';
                if (!yes) {
                    next_probe_ = std::chrono::steady_clock::now() + probe_again;
                    if (has_notify_ != false)
                        log_.notice("PGFile: no notify in the database — events of file come by NOTIFY only");
                } else if (has_notify_ == false) {
                    log_.notice("PGFile: notify is in the database now — the mode of file is read");
                }
                has_notify_ = yes;
            },
            [this](std::string_view error) {
                probing_ = false;
                next_probe_ = std::chrono::steady_clock::now() + probe_retry;
                log_.error("PGFile: probe for notify failed: {}", error);
            });
    } catch (const std::exception& e) {
        probing_ = false;
        next_probe_ = std::chrono::steady_clock::now() + probe_retry;
        log_.error("PGFile: probe for notify not sent, the pool refused the query: {}", e.what());
    }
}

// Read until the channel is single; from then on the subscriber stays for the
// life of the module. A channel returned to legacy still has rows to bring to
// their end, and notify.send alone decides which path a new event takes —
// the LISTEN on "file" serves the other one meanwhile.
//
// The lease of the channel must outlast a task: a row whose
// lease runs out is closed by the sweep as outcome unknown (`file` allows no
// repeat) while this PGFile may still be writing it. A row waits up to
// timeout + 10 s to start and runs up to timeout + 10 s more (the deadline is
// set again at the start), and its confirmation needs time after that.
// Short of it the path is not taken up.

void PGFile::read_mode()
{
    reading_mode_ = true;
    next_mode_ = std::chrono::steady_clock::now() + mode_every;

    try {
        pool_.execute(
            "SELECT mode, extract(epoch FROM lease)::bigint FROM notify.channel_policy('file')",
            [this](std::vector<PgResult> results) {
                reading_mode_ = false;

                if (modules_.stopped())
                    return;
#ifdef PGFILE_NOTIFY
                if (notify_)
                    return;
#endif
                // No row: no channel file — nothing but the path legacy.
                if (results.empty() || !results[0].ok() || results[0].rows() == 0 || results[0].columns() < 2)
                    return;

                const char* mode_value = results[0].value(0, 0);
                const std::string mode = mode_value != nullptr ? mode_value : "";

                if (mode != "single") {
                    // A NOTIFY every listener hears, and no
                    // gate to make this PGFile the only one. Legal for one
                    // backend; on a shared volume it is N writes per event.
                    if (mode == "legacy" && !gated_ && !said_ungated_) {
                        said_ungated_ = true;
                        log_.warn("PGFile: the channel file is legacy and the helper runs without the leader gate "
                                  "(process.leader) — every backend on this database writes each file; with more "
                                  "than one on a shared volume, set the channel file to single");
                    }
                    return;
                }

#ifndef PGFILE_NOTIFY
                if (refused_lease_ != -2) {
                    refused_lease_ = -2;
                    log_.error("PGFile: the channel file is single, and this build has no notify subscriber — "
                               "no event of file reaches this PGFile");
                }
                return;
#else
                const char* lease = results[0].value(0, 1);
                const long lease_s = lease != nullptr ? std::strtol(lease, nullptr, 10) : 0;
                const long floor_s = 2 * (timeout_secs_ + deadline_extra_s) + confirm_margin_s;

                if (lease_s < floor_s) {
                    // Read again every mode_every: said as an error once per
                    // lease value, then quietly, until the lease or the mode changes.
                    const bool first = lease_s != refused_lease_;
                    refused_lease_ = lease_s;
                    const auto why = fmt::format("lease {} s is shorter than 2 × (timeout + {} s) + {} s ({} s) — "
                                                 "the sweep would close a row this PGFile is still writing",
                                                 lease_s, deadline_extra_s, confirm_margin_s, floor_s);
                    if (first)
                        log_.error("PGFile: the channel file is single, its path is not taken up: {}", why);
                    else
                        log_.debug("PGFile: the channel file is single, its path is not taken up: {}", why);
                    return;
                }

                try {
                    start_notify();
                } catch (const std::exception& e) {
                    // Read again at the next mode_every: nothing half-started stays.
                    notify_.reset();
                    log_.error("PGFile: the subscriber of file not started: {}", e.what());
                }
#endif
            },
            [this](std::string_view error) {
                reading_mode_ = false;
                log_.error("PGFile: the mode of the channel file is not read: {}", error);
            });
    } catch (const std::exception& e) {
        reading_mode_ = false;
        log_.error("PGFile: the mode of the channel file is not read, the pool refused the query: {}", e.what());
    }
}

// ─── on_notify / make_task ──────────────────────────────────────────────────

void PGFile::on_notify(std::string_view /*channel*/, std::string_view payload)
{
    std::string why;
    auto task = make_task(payload, why);
    if (!task) {
        if (!why.empty())
            log_.warn("PGFile: event of file ignored: {}", why);
        return;
    }

    if (queue_.size() >= max_queue_size_) {
        log_.error("PGFile: queue full ({} tasks) — event of file {} dropped", queue_.size(), task->id);
        return;
    }

    queue_.push_back(std::move(task));
}

std::shared_ptr<PGFile::FileTask> PGFile::make_task(std::string_view payload, std::string& why) const
{
    if (payload.empty())
        return nullptr;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (...) {
        why = "malformed payload";
        return nullptr;
    }

    if (!j.is_object()) {
        why = "payload is not an object";
        return nullptr;
    }

    // Safe accessor: returns default for missing keys AND null values
    auto jstr = [&](const char* key, const char* def = "") -> std::string {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return def;
        if (!it->is_string())
            throw std::invalid_argument(fmt::format("key {} is not a string", key));
        return it->get<std::string>();
    };

    auto task = std::make_shared<FileTask>();

    try {
        task->id        = jstr("id");
        task->session   = jstr("session");
        task->operation = jstr("operation");
        task->type      = jstr("type", "-");
        task->path      = jstr("path", "/");
        task->name      = jstr("name");
        task->hash      = jstr("hash");
    } catch (const std::exception& e) {
        why = e.what();
        return nullptr;
    }
    // done/fail are NOT in the NOTIFY payload — they come from api.get_file() result

    if (task->id.empty()) {
        why = "no file id";
        return nullptr;
    }

    // Reject path traversal attempts
    if (!is_safe_path(task->path) || !is_safe_path(task->name)) {
        why = fmt::format("file {}: unsafe path", task->id);
        return nullptr;
    }

    // Build absolute path
    auto rel_path = task->path;
    if (!rel_path.empty() && rel_path.front() == '/')
        rel_path = rel_path.substr(1);

    auto dir = files_path_ / rel_path;
    task->absolute_name = (dir / task->name).string();

    task->deadline = std::chrono::steady_clock::now()
                   + std::chrono::seconds(timeout_secs_ + deadline_extra_s);

    return task;
}

// ─── heartbeat ──────────────────────────────────────────────────────────────

void PGFile::heartbeat(std::chrono::system_clock::time_point /*now*/)
{
    const auto now = std::chrono::steady_clock::now();

    if (!has_notify_.value_or(false) && !probing_ && now >= next_probe_)
        probe();

    bool read = has_notify_.value_or(false) && !reading_mode_ && now >= next_mode_;
#ifdef PGFILE_NOTIFY
    read = read && !notify_;
#endif
    if (read)
        read_mode();

    bot_.refresh_if_needed();
    process_queue();
    check_timeouts(now);
}

// ─── process_queue ──────────────────────────────────────────────────────────

void PGFile::process_queue()
{
    if (!bot_.valid())
        return;

    // Collected first: finishing a task takes it out of queue_.
    std::vector<std::shared_ptr<FileTask>> deletes;

    for (auto& task : queue_) {
        if (!task->in_progress && !task->settling) {
            task->in_progress = true;
            // Bounded from its start: a wait for the bot session is not its time.
            task->deadline = std::chrono::steady_clock::now()
                           + std::chrono::seconds(timeout_secs_ + deadline_extra_s);

            if (task->operation == "DELETE") {
                deletes.push_back(task);
            } else {
                do_file(task);
            }
        }
    }

    for (auto& task : deletes) {
        delete_file(task->absolute_name);
        finish(task, {});
    }
}

// ─── do_file ────────────────────────────────────────────────────────────────
//
// Mirrors v1 CPGFile::DoFile():
//   api.authorize(session) + api.get_file(id) → process result

void PGFile::do_file(std::shared_ptr<FileTask> task)
{
    // Use bot session for authorization, then get file
    auto sql = fmt::format(
        "SELECT * FROM api.authorize({});\n"
        "SELECT * FROM api.get_file({}::uuid)",
        pq_quote_literal(bot_.session()),
        pq_quote_literal(task->id));

    // quiet: the statement carries a session code. PgPool prints statement
    // text at debug into postgres.log — inside the container, readable
    // by any process there.
    try {
    pool_.execute(sql,
        [this, task](std::vector<PgResult> results) {
            // Failed by its deadline (or given back at the stop) meanwhile:
            // the disk is not touched, the outcome is written already.
            if (task->settling)
                return;

            // results[0] = authorize, results[1] = get_file
            if (results.size() < 2) {
                do_fail(task, "incomplete PG result");
                return;
            }

            auto& file_result = results[1];
            if (!file_result.ok() || file_result.rows() == 0) {
                do_fail(task, "file not found in database");
                return;
            }

            // Access columns by name (api.get_file returns SETOF api.file_data)
            auto col = [&](const char* name) -> std::string {
                int idx = file_result.column_index(name);
                if (idx < 0) return {};
                const char* v = file_result.value(0, idx);
                return (v && v[0] != '\0') ? std::string(v) : std::string{};
            };

            auto type = col("type");
            auto data = col("data");
            auto path = col("path");
            auto name = col("name");

            if (type.empty()) type = task->type;

            // Extract done/fail callback function names from DB result
            task->done = col("done");
            task->fail = col("fail");

            // Update task fields from DB result
            if (!path.empty()) task->path = path;
            if (!name.empty()) task->name = name;

            // Reject path traversal attempts (DB values may differ from NOTIFY)
            if (!is_safe_path(task->path) || !is_safe_path(task->name)) {
                do_fail(task, "unsafe path in file record");
                return;
            }

            // Rebuild absolute name with possibly updated path
            auto rel_path = task->path;
            if (!rel_path.empty() && rel_path.front() == '/')
                rel_path = rel_path.substr(1);
            auto dir = files_path_ / rel_path;
            task->absolute_name = (dir / task->name).string();

            if (data.empty()) {
                // No data — nothing to write
                finish(task, {});
                return;
            }

            if (type == "-") {
                // Regular file: base64 decode → write to disk
                try {
                    auto decoded = base64_decode(data);
                    auto content_type = col("mime");
                    if (content_type.empty()) content_type = "application/octet-stream";

                    delete_file(task->absolute_name);
                    if (!write_file(task->absolute_name, decoded)) {
                        do_fail(task, "failed to write file: " + task->absolute_name);
                        return;
                    }

                    do_done(task, decoded, content_type);
                } catch (const std::exception& e) {
                    do_fail(task, fmt::format("decode error: {}", e.what()));
                }
            } else if (type == "l") {
                // Link: data is base64-encoded URL
                try {
                    auto url = base64_decode(data);
                    if (url.starts_with("https://") || url.starts_with("http://")) {
                        do_curl(task, url);
                    } else {
                        do_fail(task, "invalid URL in file data");
                    }
                } catch (const std::exception& e) {
                    do_fail(task, fmt::format("decode URL error: {}", e.what()));
                }
            } else if (type == "s") {
                // Storage (S3): delete local copy — S3 is authoritative
                delete_file(task->absolute_name);
                finish(task, {});
            } else {
                do_fail(task, "unknown file type: " + type);
            }
        },
        [this, task](std::string_view error) {
            do_fail(task, fmt::format("PG error: {}", error));
        },
        /*quiet=*/true,
        // authorize + get_file read and change nothing: sent again after a
        // lost connection.
        PgRetry::if_lost);
    } catch (const std::exception& e) {
        // The pool refused: not started — the next heartbeat tries
        // again; on the path single a row still not started by its deadline
        // is given back.
        task->in_progress = false;
        log_.error("PGFile: file {}: not read, the pool refused the query: {}", task->id, e.what());
    }
}

// ─── do_curl ────────────────────────────────────────────────────────────────

void PGFile::do_curl(std::shared_ptr<FileTask> task, const std::string& url)
{
    fetch_.get(url, {},
        // on_done
        [this, task](FetchResponse resp) {
            if (resp.status_code == 200) {
                // Failed by its deadline meanwhile: the disk is not touched.
                if (task->settling)
                    return;

                delete_file(task->absolute_name);
                if (!write_file(task->absolute_name, resp.body)) {
                    do_fail(task, "failed to write fetched file");
                    return;
                }

                // Determine content type from response headers
                std::string content_type = "application/octet-stream";
                for (const auto& [k, v] : resp.headers) {
                    if (k == "Content-Type" || k == "content-type") {
                        content_type = v;
                        break;
                    }
                }

                do_done(task, resp.body, content_type);
            } else {
                do_fail(task, fmt::format("HTTP {} fetching file", resp.status_code));
            }
        },
        // on_error
        [this, task](std::string_view error) {
            do_fail(task, fmt::format("fetch error: {}", error));
        });
}

// ─── do_done ────────────────────────────────────────────────────────────────
//
// Call the "done" PG callback: done_func(id, path, size, hash, content_type)

void PGFile::do_done(std::shared_ptr<FileTask> task,
                     std::string_view body, std::string_view content_type)
{
    // One outcome per task: failed by its deadline meanwhile.
    if (task->settling)
        return;

    if (task->done.empty()) {
        // No callback configured — nothing to write but the end
        finish(task, {});
        return;
    }

    auto hash = sha256_hex(body);
    auto sql = fmt::format(
        "SELECT {}({}, {}, {}, {}, {})",
        task->done,
        pq_quote_literal(task->id),
        pq_quote_literal(task->absolute_name),
        body.size(),
        pq_quote_literal(hash),
        pq_quote_literal(std::string(content_type)));

    // Path single: a done callback that throws takes the confirmation down
    // with it; the fail callback is written instead, so that the row ends.
    Fallback fallback;
    if (!task->fail.empty()) {
        fallback = [task](std::string_view error) {
            return fmt::format("SELECT {}({}, {})", task->fail, pq_quote_literal(task->id),
                               pq_quote_literal(fmt::format("done callback rolled back: {}", error)));
        };
    }

    finish(task, std::move(sql), std::move(fallback));
}

// ─── do_fail ────────────────────────────────────────────────────────────────
//
// Call the "fail" PG callback: fail_func(id, message)

void PGFile::do_fail(std::shared_ptr<FileTask> task, std::string_view message)
{
    // One outcome per task.
    if (task->settling)
        return;

    log_.warn("PGFile: file {}: {}", task->id, message);

    if (task->fail.empty()) {
        // No callback configured — nothing to write but the end
        finish(task, {});
        return;
    }

    auto sql = fmt::format("SELECT {}({}, {})",
                           task->fail,
                           pq_quote_literal(task->id),
                           pq_quote_literal(std::string(message)));

    finish(task, std::move(sql));
}

// ─── finish ─────────────────────────────────────────────────────────────────

void PGFile::finish(const std::shared_ptr<FileTask>& task, std::string sql, Fallback fallback)
{
    // One end per task: a second would race the first (and on the path
    // single follow a nack with an ack).
    if (task->settling)
        return;
    task->settling = true;

#ifdef PGFILE_NOTIFY
    if (task->row) {
        settle_single(task, std::move(sql), std::move(fallback));
        return;
    }
#endif
    (void)fallback;

    if (sql.empty()) {
        remove_task(task);
        return;
    }

    try {
        pool_.execute(sql,
            [this, task](std::vector<PgResult> /*results*/) {
                remove_task(task);
            },
            [this, task](std::string_view error) {
                log_.error("PGFile: file {}: callback failed: {}", task->id, error);
                remove_task(task);
            },
            /*quiet=*/true);
    } catch (const std::exception& e) {
        log_.error("PGFile: file {}: callback not sent, the pool refused the query: {}", task->id, e.what());
        remove_task(task);
    }
}

// ─── check_timeouts ─────────────────────────────────────────────────────────

void PGFile::check_timeouts(std::chrono::steady_clock::time_point now)
{
    // Collected first: finishing a task takes it out of queue_.
    std::vector<std::shared_ptr<FileTask>> timed_out;
#ifdef PGFILE_NOTIFY
    std::vector<std::shared_ptr<FileTask>> not_started;
#endif

    for (auto& task : queue_) {
        if (task->settling || now < task->deadline)
            continue;
        if (task->in_progress)
            timed_out.push_back(task);
#ifdef PGFILE_NOTIFY
        // Taken, never started (no bot session all this time): another
        // PGFile may have one. A row kept to its lease would be closed by the
        // sweep as outcome unknown — `file` allows no repeat.
        else if (task->row)
            not_started.push_back(task);
#endif
    }

    for (auto& task : timed_out)
        do_fail(task, "file operation timeout");

#ifdef PGFILE_NOTIFY
    for (auto& task : not_started) {
        log_.warn("PGFile: file {}: not started within timeout + {} s (no bot session) — given back",
                  task->id, deadline_extra_s);
        give_back(task, "unsent: not started in time, no bot session");
    }
#endif
}

// ─── remove_task ────────────────────────────────────────────────────────────
//
// By the task, not by the file id: two events of one file —
// two NOTIFYs, or two rows of notify.queue — are two tasks, and the end of one
// must not take the other out.

void PGFile::remove_task(const std::shared_ptr<FileTask>& task)
{
    queue_.erase(std::remove(queue_.begin(), queue_.end(), task), queue_.end());
}

#ifdef PGFILE_NOTIFY

// ─── path single: the subscriber ────────────────────────────────────────────

void PGFile::start_notify()
{
    db_platform::NotifySubscriber::Options options;
    options.channel     = "file";
    options.node        = node_;
    options.application = "PGFile";
    options.role        = "helper";

    notify_ = std::make_unique<db_platform::NotifySubscriber>(pool_, loop_, log_, std::move(options),
        [this](const db_platform::NotifyRow& row) {
            try {
                on_row(row);
            } catch (const std::exception& e) {
                // The subscriber's callback is not the place to throw; the sweep closes the row by its lease.
                log_.error("PGFile: row {} of file not handled: {}", row.id, e.what());
            }
        });

    log_.notice("PGFile: the channel file is single — events are taken from notify.queue");
    notify_->start();
}

// ─── path single: a row ─────────────────────────────────────────────────────

void PGFile::on_row(const db_platform::NotifyRow& row)
{
    if (modules_.stopped()) {
        notify_->refuse(row, "unsent: stopping, taken after the stop began");
        return;
    }

    // The same row handed out again while this module still has it: whatever
    // outcome is written here goes out under the new token.
    for (const auto& t : queue_) {
        if (t->row && t->row->id == row.id) {
            log_.warn("PGFile: file {}: row {} delivered again (attempt {}) while still here — "
                      "its outcome goes out under the new delivery", t->id, row.id, row.attempts);
            t->row = row;
            return;
        }
    }

    // The payload is the event the NOTIFY would carry (db.ft_file_notify).
    std::string why;
    auto task = make_task(row.payload, why);
    if (!task) {
        // Nothing to act on (FR.02): refused, not repeated (`file` allows
        // none) — closed with the reason kept in the row.
        if (why.empty())
            why = "empty payload";
        log_.warn("PGFile: row {} of file ignored: {}", row.id, why);
        try {
            notify_->refuse(row, "malformed: " + why);
        } catch (const std::exception& e) {
            log_.error("PGFile: row {} of file not refused, the pool refused — the sweep closes it by its lease: {}",
                       row.id, e.what());
        }
        return;
    }

    // A row taken is never left without an end. Not worked
    // on — given back; on its first delivery it waits for another PGFile.
    if (queue_.size() >= max_queue_size_) {
        log_.error("PGFile: queue full ({} tasks) — row {} of file {} given back", queue_.size(), row.id, task->id);
        try {
            notify_->refuse(row, fmt::format("unsent: queue full ({} tasks)", queue_.size()));
        } catch (const std::exception& e) {
            log_.error("PGFile: row {} of file not given back, the pool refused: {}", row.id, e.what());
        }
        return;
    }

    task->row = row;
    queue_.push_back(std::move(task));
}

// ─── path single: the outcome ───────────────────────────────────────────────
//
// The callback and the confirmation are one transaction (notify.ack, last):
// under a token that is not current the whole of it is rolled back.
//
// The first send goes through the subscriber, which takes the row off its
// list. The subscriber does not send again after a lost connection, and a
// failure there may be either that or a statement that failed: the same
// statement is sent once more by the module itself, on a pool that resends
// after a lost connection — committed the first time, it is refused by the
// confirmation (NT409) and changes nothing. Failing again, it is a statement
// that fails: its fallback, if it has one, is written instead, and failing
// that — the confirmation alone, so that the row ends.

void PGFile::settle_single(std::shared_ptr<FileTask> task, std::string sql, Fallback fallback)
{
    task->settling = true;
    const auto row = *task->row;

    if (!notify_) {
        settle_again(std::move(task), row, std::move(sql), std::move(fallback), false);
        return;
    }

    auto on_done = [this, task](std::vector<PgResult>) { remove_task(task); };
    auto on_fail = [this, task, row, sql, fallback](std::string_view error, bool stale) {
        if (stale) {
            settle_stale(task, row, sql, fallback);
            return;
        }
        log_.warn("PGFile: file {}: outcome and confirmation not written ({}) — sent again", task->id, error);
        settle_again(task, row, sql, fallback, false);
    };

    try {
        // quiet: as on the path legacy.
        notify_->complete(row, sql, std::move(on_done), std::move(on_fail), /*quiet=*/true);
    } catch (const std::exception& e) {
        log_.error("PGFile: file {}: outcome not sent, the pool refused — the sweep closes the row by its lease: {}",
                   task->id, e.what());
        remove_task(task);
    }
}

void PGFile::settle_again(std::shared_ptr<FileTask> task, db_platform::NotifyRow row, std::string sql,
                          Fallback fallback, bool last)
{
    auto statement = db_platform::NotifySubscriber::ack_sql(row, sql);

    auto on_result = [this, task, row, sql, fallback, last](std::vector<PgResult> results) {
        const auto bad = std::find_if(results.begin(), results.end(),
            [](const PgResult& r) { return !r.ok(); });

        if (bad == results.end()) {
            remove_task(task);
            return;
        }

        if (std::string_view(bad->sqlstate()) == "NT409") {
            settle_stale(task, row, sql, fallback);
            return;
        }

        const std::string error(bad->error_message());
        // The connection or the server went: written or not is unknown — the
        // fail callback beside a file maybe confirmed done would be a lie.
        const std::string_view state(bad->sqlstate());
        const bool unknown = state.empty() || state.starts_with("08") || state.starts_with("57P0");
        if (!last && !unknown) {
            log_.error("PGFile: file {}: callback rolled back: {}", task->id, error);
            settle_again(task, row, fallback ? fallback(error) : std::string{}, {}, true);
            return;
        }

        // Delivered again to this module while the last send was out: the
        // row is held under the new token now.
        if (task->row && task->row->token != row.token) {
            settle_stale(task, row, sql, fallback);
            return;
        }

        log_.error("PGFile: file {}: outcome not written, the sweep closes its row by the lease: {}", task->id, error);
        remove_task(task);
    };

    try {
        pool_.execute(std::move(statement), std::move(on_result), {}, /*quiet=*/true, PgRetry::if_lost);
    } catch (const std::exception& e) {
        log_.error("PGFile: file {}: outcome not sent, the pool refused — the sweep closes the row by its lease: {}",
                   task->id, e.what());
        remove_task(task);
    }
}

// NT409: what was sent with the confirmation is rolled back. Either an
// earlier send of this very outcome committed (a resend after a lost
// connection), or the row is not this delivery's any more — or, delivered
// again meanwhile, it is this module's under a new token.
void PGFile::settle_stale(std::shared_ptr<FileTask> task, const db_platform::NotifyRow& row,
                          std::string sql, Fallback fallback)
{
    if (task->row && task->row->token != row.token) {
        log_.notice("PGFile: file {}: delivered again meanwhile — the outcome goes out under the new delivery",
                    task->id);
        settle_single(std::move(task), std::move(sql), std::move(fallback));
        return;
    }

    log_.warn("PGFile: file {}: the row is not this delivery's any more — the outcome written here is "
              "rolled back (written already, or closed by the sweep)", task->id);
    remove_task(task);
}

// A row taken and not worked on, given back: notify.nack with a reason that
// starts with `unsent:`. On its first delivery the row goes back to the queue
// whatever max_attempts says (`file` has none) and another PGFile takes it.
void PGFile::give_back(const std::shared_ptr<FileTask>& task, std::string_view reason)
{
    task->settling = true;
    if (notify_ && task->row) {
        try {
            notify_->refuse(*task->row, reason);
        } catch (const std::exception& e) {
            log_.error("PGFile: file {}: row not given back, the pool refused — the sweep closes it by its lease: {}",
                       task->id, e.what());
        }
    }
    remove_task(task);
}

// ─── path single: the stop ──────────────────────────────────────────────────

void PGFile::on_stopping()
{
    if (!notify_)
        return;

    // Not chosen as an addressee any more, nothing new taken.
    notify_->drain();

    // Rows taken and not started: given back, another PGFile writes them.
    std::vector<std::shared_ptr<FileTask>> waiting;
    for (const auto& t : queue_) {
        if (t->row && !t->in_progress && !t->settling)
            waiting.push_back(t);
    }
    for (const auto& t : waiting)
        give_back(t, "unsent: stopping, not started");
}

// A row being worked on: the stop waits for it (up to process.stop_timeout;
// a download ends by the timeout).
bool PGFile::busy() const
{
    return std::any_of(queue_.begin(), queue_.end(), [](const auto& t) {
        return t->row && t->in_progress && !t->settling;
    });
}

// What busy() did not see to its end is given back as well: the work of a
// PGFile is repeatable — the next one reads db.file again, deletes and
// writes the same file, nothing has left this host — and a row kept to its
// lease would be closed by the sweep, the disk copy stale. A row
// whose confirmation is on the way is left alone. In one batch on the
// listener, as the subscriber does it: the pool's connections may be held by
// work that hangs.
void PGFile::stop_notify()
{
    if (!notify_)
        return;

    const int number = notify_->number();

    std::string sql;
    if (number != 0)
        sql += fmt::format("SELECT notify.drain({}); ", number);

    std::size_t refused = 0;
    const auto snapshot = queue_;
    for (const auto& task : snapshot) {
        if (!task->row || task->settling)
            continue;

        sql += fmt::format(
            "DO $pgfile$ BEGIN PERFORM notify.nack({}, {}, 'unsent: stopping, the file not finished'); "
            "EXCEPTION WHEN SQLSTATE 'NT409' THEN NULL; END $pgfile$; ",
            task->row->id, task->row->token);
        ++refused;
        task->settling = true;
        remove_task(task);
    }

    if (number != 0)
        sql += fmt::format("SELECT notify.unsubscribe({})", number);

    if (!sql.empty()) {
        log_.notice("PGFile: subscriber {} of file stopping — {} unfinished row(s) given back, deregistering",
                    number, refused);
        try {
            pool_.listener_execute(std::move(sql),
                [this](std::vector<PgResult> results) {
                    const auto bad = std::find_if(results.begin(), results.end(),
                        [](const PgResult& r) { return !r.ok(); });
                    if (bad != results.end())
                        log_.error("PGFile: the subscriber of file did not stop cleanly: {}", bad->error_message());
                },
                {}, /*quiet=*/false);
        } catch (const std::exception& e) {
            // The rows return by their lease, the subscriber is forgotten by
            // its heartbeat: nothing here outlives the process.
            log_.error("PGFile: the subscriber of file not deregistered, the pool refused: {}", e.what());
        }
    }

    notify_.reset();
}

#endif // PGFILE_NOTIFY

} // namespace apostol

#endif // WITH_POSTGRESQL
