#ifdef WITH_POSTGRESQL

#include "PGFile.hpp"
#include "apostol/application.hpp"

#include "apostol/base64.hpp"
#include "apostol/file_utils.hpp"
#include "apostol/pg_utils.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace apostol
{

// ─── Construction ────────────────────────────────────────────────────────────

PGFile::PGFile(Application& app, EventLoop& loop)
    : pool_(app.db_pool())
    , fetch_(loop)
    , bot_(app.db_pool(), "PGFile/2.0", "127.0.0.1")
    , enabled_(true)
{
    // Read path and timeout from config
    std::string path_str;
    int timeout = 60;
    if (auto* cfg = app.module_config("PGFile")) {
        if (cfg->contains("path"))
            path_str = (*cfg)["path"].get<std::string>();
        if (cfg->contains("timeout") && (*cfg)["timeout"].is_number())
            timeout = (*cfg)["timeout"].get<int>();
    }

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
    pool_.listen("file", [this](std::string_view ch, std::string_view payload) {
        try {
            on_notify(ch, payload);
        } catch (const std::exception& e) {
            // Never let exceptions escape from NOTIFY callback
            (void)e;
        }
    });
}

void PGFile::on_stop()
{
    pool_.unlisten("file");
    bot_.sign_out();
}

// ─── on_notify ──────────────────────────────────────────────────────────────

void PGFile::on_notify(std::string_view /*channel*/, std::string_view payload)
{
    if (payload.empty())
        return;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (...) {
        return; // malformed payload
    }

    // Safe accessor: returns default for missing keys AND null values
    auto jstr = [&](const char* key, const char* def = "") -> std::string {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return def;
        return it->get<std::string>();
    };

    auto task = std::make_shared<FileTask>();

    task->id        = jstr("id");
    task->session   = jstr("session");
    task->operation = jstr("operation");
    task->type      = jstr("type", "-");
    task->path      = jstr("path", "/");
    task->name      = jstr("name");
    task->hash      = jstr("hash");
    // done/fail are NOT in the NOTIFY payload — they come from api.get_file() result

    if (task->id.empty())
        return;

    // Reject path traversal attempts
    if (!is_safe_path(task->path) || !is_safe_path(task->name))
        return;

    // Build absolute path
    auto rel_path = task->path;
    if (!rel_path.empty() && rel_path.front() == '/')
        rel_path = rel_path.substr(1);

    auto dir = files_path_ / rel_path;
    task->absolute_name = (dir / task->name).string();

    task->deadline = std::chrono::steady_clock::now()
                   + std::chrono::seconds(timeout_secs_ + 10);

    if (queue_.size() >= max_queue_size_)
        return; // drop task — queue full

    queue_.push_back(std::move(task));
}

// ─── heartbeat ──────────────────────────────────────────────────────────────

void PGFile::heartbeat(std::chrono::system_clock::time_point /*now*/)
{
    bot_.refresh_if_needed();
    process_queue();
    check_timeouts(std::chrono::steady_clock::now());
}

// ─── process_queue ──────────────────────────────────────────────────────────

void PGFile::process_queue()
{
    if (!bot_.valid())
        return;

    // Collect DELETE task ids first (modifying queue_ during iteration is unsafe)
    std::vector<std::string> delete_ids;

    for (auto& task : queue_) {
        if (!task->in_progress) {
            task->in_progress = true;

            if (task->operation == "DELETE") {
                delete_file(task->absolute_name);
                delete_ids.push_back(task->id);
            } else {
                do_file(task);
            }
        }
    }

    for (auto& id : delete_ids)
        remove_task(id);
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

    pool_.execute(sql,
        [this, task](std::vector<PgResult> results) {
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
                remove_task(task->id);
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
                remove_task(task->id);
            } else {
                do_fail(task, "unknown file type: " + type);
            }
        },
        [this, task](std::string_view error) {
            do_fail(task, fmt::format("PG error: {}", error));
        });
}

// ─── do_curl ────────────────────────────────────────────────────────────────

void PGFile::do_curl(std::shared_ptr<FileTask> task, const std::string& url)
{
    fetch_.get(url, {},
        // on_done
        [this, task](FetchResponse resp) {
            if (resp.status_code == 200) {
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
    if (task->done.empty()) {
        // No callback configured — just remove the task
        remove_task(task->id);
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

    pool_.execute(sql,
        [this, task](std::vector<PgResult> /*results*/) {
            remove_task(task->id);
        },
        [this, task](std::string_view /*error*/) {
            remove_task(task->id);
        },
        /*quiet=*/true);
}

// ─── do_fail ────────────────────────────────────────────────────────────────
//
// Call the "fail" PG callback: fail_func(id, message)

void PGFile::do_fail(std::shared_ptr<FileTask> task, std::string_view message)
{
    if (task->fail.empty()) {
        // No callback configured — just remove the task
        remove_task(task->id);
        return;
    }

    auto sql = fmt::format("SELECT {}({}, {})",
                           task->fail,
                           pq_quote_literal(task->id),
                           pq_quote_literal(std::string(message)));

    pool_.execute(sql,
        [this, task](std::vector<PgResult> /*results*/) {
            remove_task(task->id);
        },
        [this, task](std::string_view /*error*/) {
            remove_task(task->id);
        },
        /*quiet=*/true);
}

// ─── check_timeouts ─────────────────────────────────────────────────────────

void PGFile::check_timeouts(std::chrono::steady_clock::time_point now)
{
    std::vector<std::shared_ptr<FileTask>> timed_out;

    for (auto& task : queue_) {
        if (task->in_progress && now >= task->deadline)
            timed_out.push_back(task);
    }

    for (auto& task : timed_out)
        do_fail(task, "file operation timeout");
}

// ─── remove_task ────────────────────────────────────────────────────────────

void PGFile::remove_task(const std::string& id)
{
    queue_.erase(
        std::remove_if(queue_.begin(), queue_.end(),
            [&id](const auto& t) { return t->id == id; }),
        queue_.end());
}

} // namespace apostol

#endif // WITH_POSTGRESQL
