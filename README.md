[![ru](https://img.shields.io/badge/lang-ru-green.svg)](README.ru-RU.md)

Postgres File
-

**Module** for **Apostol CRM**[^crm].

Description
-
**PGFile** synchronizes files between PostgreSQL and the local filesystem. It subscribes to PostgreSQL `LISTEN/NOTIFY` notifications from the `file` module of [db-platform](https://github.com/apostoldevel/db-platform) and writes, updates, or removes physical files on disk whenever database records change.

The module runs as a long-lived **Helper** inside the Apostol helper process, sharing the same `epoll`-based event loop — no threads, no blocking I/O.

> **PGFile** and **FileServer** are complementary modules. PGFile populates the filesystem from the database; FileServer serves those files over HTTP.

How it works
-
The `db.file` table has an `AFTER INSERT/UPDATE/DELETE` trigger (`t_file_notify`) that sends a `pg_notify('file', payload)` notification with:

```json
{"session": "...", "operation": "INSERT|UPDATE|DELETE", "id": "uuid", "type": "-|l|s", "name": "...", "path": "...", "hash": "..."}
```

PGFile:
1. Subscribes to the `file` PostgreSQL notify channel via `LISTEN file`.
2. On notification — parses the payload and enqueues a file task.
3. Based on the operation and file type — writes, replaces, downloads, or removes the file.

Several backends on one database
-
The trigger sends through `notify.send('file', …)` of db-platform, and the mode of the notify channel `file` decides who hears it:

- `legacy` (the default) or `broadcast` — a `NOTIFY` on `file` that every PGFile hears. Right for one backend, or for backends with a disk each. With several backends on a **shared** volume, every one writes each file — unless the helper runs under the leader gate (`process.leader`); PGFile says so in the log when the channel is `legacy` and the gate is off.
- `single` — a row of `notify.queue` handed to exactly one PGFile (the common channel is silent). PGFile reads the mode every 10 s and, once it is `single`, takes rows through `NotifySubscriber` for the life of the process. The `done`/`fail` callback and the confirmation (`notify.ack`) are one transaction. A row not worked on — the queue is full, the stop began, no bot session until the deadline, or the work not finished at the stop — is given back with `notify.nack` and a reason that starts with `unsent:`; on its first delivery another PGFile takes it. A row that arrived while no PGFile listened waits in the queue (no event is lost by a restart).

Switch `file` to `single` only when **every** backend on the database has a PGFile with this path: an older one hears nothing in that mode. The path is taken up only when the channel's `lease` is at least `2 × (timeout + 10 s) + 20 s`; otherwise PGFile logs why and stays on `LISTEN`.

File types
-
| Type | DB meaning | PGFile action |
|------|-----------|---------------|
| `-`  | Regular file | Decode base64 content from `data` column, write to filesystem. |
| `l`  | Symbolic link (URL) | The `data` column contains an HTTP/HTTPS URL; fetch the remote resource via cURL and save it. |
| `s`  | Storage (S3) | Remove the local copy from filesystem — S3 is now the authoritative store. |
| `d`  | Directory | Not handled directly; directories are created on demand when writing files. |

On **UPDATE**, if the file path or content hash has changed, the old file is removed before the new one is written.

On **DELETE**, the physical file is removed from disk without fetching the database record.

S3 upload flow
-
Files can be uploaded to S3 from the database layer using `PutFileToS3(id, region, done, fail)`:

1. Reads file content and metadata from `db.file`.
2. Reads S3 config from the registry (`CONFIG\S3`: Region, Endpoint, AccessKey, SecretKey).
3. Builds an **AWS Signature Version 4** (HMAC-SHA256) authorization header entirely in PL/pgSQL.
4. Calls `http.fetch(...)` — **PGFetch** sends the `PUT` request to S3 asynchronously.
5. On completion, the `done` callback updates the `db.file` record (typically sets `type = 's'`, removing the `data` blob).
6. The `t_file_notify` trigger fires with `type = 's'` → PGFile receives the notification and **removes the local copy** (S3 is now the store).

If the root directory is named `public`, the `x-amz-acl: public-read` header is added automatically.

Callbacks
-
The `db.file` record may specify `done` and `fail` columns — fully qualified PL/pgSQL function names (`schema.function`) called after a successful file operation or on failure, respectively.

For **URL downloads** (type `l`), the callback is invoked after the remote resource has been saved to local disk, with the following signature:

```sql
CREATE OR REPLACE FUNCTION schema.my_done (
  pId           uuid,    -- file id from db.file
  pAbsoluteName text,    -- absolute local filesystem path
  pSize         integer, -- file size in bytes
  pHash         text,    -- SHA-256 hash of the content
  pMime         text     -- detected MIME type
) RETURNS void ...
```

A typical `done` callback uses this local file info to trigger the next step in the pipeline — for example, calling `PutFileToS3` to upload the downloaded file to object storage.

Configuration
-
Enable the module in the Apostol configuration file:

```json
{
  "modules": {
    "PGFile": {
      "enabled": true
    }
  }
}
```

Related modules
-
- **[FileServer](https://github.com/apostoldevel/module-FileServer)** — serves files managed by PGFile over HTTP (`GET /file/...`)
- **[PGFetch](https://github.com/apostoldevel/module-PGFetch)** — required for S3 uploads: `PutFileToS3` uses `http.fetch` to PUT files to S3 asynchronously
- **db-platform `file` module** — database layer: `db.file` table, `api.get_file`, `PutFileToS3`, REST endpoints, `t_file_notify` trigger

Installation
-
Follow the build and installation instructions for [Apostol (C++20)](https://github.com/apostoldevel/libapostol#build-and-installation).

[^crm]: **Apostol CRM** — a template project built on the [A-POST-OL](https://github.com/apostoldevel/libapostol) (C++20) and [PostgreSQL Framework for Backend Development](https://github.com/apostoldevel/db-platform) frameworks.
