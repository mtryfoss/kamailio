# db_nats reference backend

Terminates Kamailio `db_nats` requests against MySQL.

This is a reference implementation shipped alongside the `db_nats` module. It
is deliberately not wired into the Kamailio build: there is no Go toolchain in
the C build, so build and deploy it separately.

## Build and run

```
go build -o db_nats_backend .
cp config.sample.yaml config.yaml   # then edit it
./db_nats_backend -config config.yaml
```

## Subjects

Three subjects under the configured prefix, matching what the module publishes:

| Subject            | Purpose                                       |
| ------------------ | --------------------------------------------- |
| `<prefix>.op`      | structured operations                         |
| `<prefix>.raw`     | verbatim SQL passthrough, disabled by default |
| `<prefix>.cursor`  | fetch continuation and cursor close           |

They are separate so NATS account permissions can deny the raw path per
credential. Subscriptions use a queue group, so backends scale horizontally.

Each instance additionally takes a private, non-queue subscription on
`<prefix>.cursor.<instance>`, and returns that subject as `cursor_subject`
when it opens a cursor. An open cursor holds a live `*sql.Rows` in one
process, so continuation has to come back to its owner rather than being
load balanced across the group.

**One queue group must serve exactly one logical database.** Operations are
load balanced, so instances in a group pointing at different databases would
answer the same query from different data. Give each database its own subject
prefix. Read replicas are equally unsuitable, since a cursor is bound to one
connection's snapshot.

## Shutdown

On `SIGTERM`/`SIGINT` the backend drains rather than exiting immediately:

1. queue subscriptions are drained, so the group routes new work to the other
   instances;
2. the private `<prefix>.cursor.<instance>` subscription stays up until every
   open cursor finishes or `shutdown.grace` expires;
3. in-flight handlers are awaited before any `sql.Rows` is closed.

This matters because a client paging a large table would otherwise lose its
cursor on every restart, and at startup a failed usrloc preload is fatal.

## Container image

`Dockerfile` builds a static, distroless, non-root image. It is standalone and
not wired into the Kamailio build, which has no Go toolchain.

```
docker build -t db_nats_backend .
docker run --rm -v $PWD/config.yaml:/etc/db_nats/config.yaml:ro db_nats_backend
```

The image has no shell or package manager, so configuration is mounted rather
than baked in. `tzdata` is embedded in the binary, so `mysql.timezone` works
despite the image carrying no system zoneinfo.

## Wire protocol

Values travel as typed JSON and the backend binds them as query parameters, so
no value is ever concatenated into SQL. Identifiers cannot be parameters, so
every table, column and ORDER BY name is validated against the grants and
matched by `identOK` before it is backtick-quoted.

Two encoding decisions are load-bearing:

- **Every datum is a JSON string, never a JSON number.** The C side parses with
  srjson, which stores all numbers as `double`; a 64-bit integer sent as a JSON
  number would be silently corrupted above 2^53.
- **`datetime` is unix epoch seconds in both directions.** The alternative,
  `db_str2time()`, parses a SQL datetime string through `mktime()` in local
  time, which is where the historical DST bug in this area comes from.

A SQL NULL is `null` in a row array, which is what the C decoder recognises.

## Column types

`wireTypeFor` mirrors `db_mysql_get_columns()` in
`src/modules/db_mysql/km_res.c`, so swapping drivers does not change the
`db_val_t` types a module sees. Two points worth knowing:

- Text and unrecognised types map to `string` (`DB1_STRING`), never `str`
  (`DB1_STR`). `db_str2val()` only NUL-terminates the `DB1_STRING` copy, and
  usrloc and auth_db read values with `VAL_STRING()` + `strlen()`.
- `TIMESTAMP` maps to `datetime`, where db_mysql maps it to `DB1_INT`. That
  db_mysql mapping cannot work in practice, since MySQL returns a formatted
  datetime string that `db_str2val()` would fail to parse as an integer.

## Transactions

Not supported, by design. `START TRANSACTION` / `COMMIT` / `SET AUTOCOMMIT` and
friends need a connection that persists across calls; over stateless
request/reply each statement can land on a different backend and a different
MySQL connection. The module refuses them (`session_sql_mode`), and this
backend refuses them again in case another publisher reaches the subject.

This is why `ims_usrloc_scscf` and `p_usrloc` are not usable with `db_nats` —
see the module README.
