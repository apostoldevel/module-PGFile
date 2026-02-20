[![ru](https://img.shields.io/badge/lang-ru-green.svg)](https://github.com/apostoldevel/module-PGFile/blob/master/README.ru-RU.md)

Postgres File
-
**PGFile** is a module for [Apostol](https://github.com/apostoldevel/apostol).

Description
-
**PGFile** synchronizes files between PostgreSQL and the local filesystem. It subscribes to PostgreSQL `LISTEN/NOTIFY` notifications from the `file` module of [db-platform](https://github.com/apostoldevel/db-platform) and writes, updates, or removes physical files on disk whenever database records change.

The module runs as a long-lived **Helper** inside the Apostol helper process, sharing the same `epoll`-based event loop — no threads, no blocking I/O.

> **PGFile** and **FileServer** are complementary modules that share the `FileCommon` base class. PGFile populates the filesystem from the database; FileServer serves those files over HTTP.

How it works
-
The `db.file` table has an `AFTER INSERT/UPDATE/DELETE` trigger (`t_file_notify`) that sends a `pg_notify('file', payload)` notification with:

```json
{"session": "...", "operation": "INSERT|UPDATE|DELETE", "id": "uuid", "type": "-|l|s", "name": "...", "path": "...", "hash": "..."}
```

PGFile:
1. Subscribes to the `file` PostgreSQL notify channel via `LISTEN file`.
2. On notification — fetches the full file record from the database via `api.get_file(id)`.
3. Based on the operation and file type — writes, replaces, downloads, or removes the file.

File types
-
| Type | DB meaning | PGFile action |
|------|-----------|---------------|
| `-`  | Regular file | Decode base64 content from `data` column, write to filesystem. |
| `l`  | Symbolic link (URL) | The `data` column contains an HTTP/HTTPS URL; fetch the remote resource via the built-in HTTP client or cURL and save it. |
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

```ini
[module/PGFile]
enable=true
```

Related modules
-
- **FileServer** — serves files managed by PGFile over HTTP (`GET /file/<uuid>/...`)
- **PGFetch** — required for S3 uploads: `PutFileToS3` uses `http.fetch` to PUT files to S3 asynchronously
- **db-platform `file` module** — database layer: `db.file` table, `api.get_file`, `PutFileToS3`, REST endpoints, `t_file_notify` trigger
- **FileCommon** (`src/common/FileCommon`) — shared C++ base class: authentication, queue, `CFileHandler`, cURL support

Installation
-
Follow the build and installation instructions for [Apostol](https://github.com/apostoldevel/apostol#build-and-installation).
