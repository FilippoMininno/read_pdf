![Build Status](https://img.shields.io/github/actions/workflow/status/FilippoMininno/read_pdf/MainDistributionPipeline.yml)
# read_pdf

A [DuckDB](https://duckdb.org) extension that reads AcroForm fields from PDF files into tables.

Built on top of [QPDF](https://github.com/qpdf/qpdf).

---

## Usage

`read_pdf` is a table function. The first argument is a file path, a glob pattern, or a directory.

```sql
-- Single file
SELECT * FROM read_pdf('form.pdf');

-- Glob pattern
SELECT * FROM read_pdf('/data/forms/*.pdf');

-- Directory (scans all *.pdf files inside)
SELECT * FROM read_pdf('/data/forms/');
```

The schema is derived from the AcroForm fields of the first PDF found. Each row represents one PDF file. The first column is always `source_file` (the filename), followed by one column per form field.

Named parameters:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `password` | VARCHAR | — | Password for encrypted PDFs. Overrides any matching secret. |
| `ignore_errors` | BOOLEAN | `false` | Skip unreadable or unparseable files instead of aborting. |

### Basic example

Given a PDF with fields `Name_First` and `Name_Last`:

```sql
SELECT source_file, Name_First, Name_Last FROM read_pdf('pdf/filled.pdf');
```

```
┌─────────────┬────────────┬───────────┐
│ source_file │ Name_First │ Name_Last │
│   varchar   │  varchar   │  varchar  │
├─────────────┼────────────┼───────────┤
│ filled.pdf  │ Merluzzo   │ Maurizio  │
└─────────────┴────────────┴───────────┘
```

Empty or missing fields are returned as `NULL`.

---

## Encrypted PDFs

### Via the Secret Manager (recommended)

Register a secret scoped to a directory or file path so you don't have to pass the password in every query:

```sql
-- Create a temporary in-memory secret (recommended — not written to disk).
CREATE TEMPORARY SECRET my_pdf_secret (
    TYPE pdf,
    password 'correct-horse-battery-staple',
    SCOPE '/data/confidential/'
);

-- The password is resolved automatically from the matching secret.
SELECT * FROM read_pdf('/data/confidential/report.pdf');
```

Secrets are matched by the longest-prefix scope, so you can register different passwords for different directories:

```sql
CREATE TEMPORARY SECRET dept_a (TYPE pdf, password 'passA', SCOPE '/data/dept_a/');
CREATE TEMPORARY SECRET dept_b (TYPE pdf, password 'passB', SCOPE '/data/dept_b/');

SELECT * FROM read_pdf('/data/dept_a/form.pdf'); -- uses passA
SELECT * FROM read_pdf('/data/dept_b/form.pdf'); -- uses passB
```

> **Note on persistence:** `CREATE SECRET` (without `TEMPORARY`) writes secrets to disk in plain text. Use `CREATE TEMPORARY SECRET` for passwords — they live only for the duration of the session.

### Via the `password` parameter

The `password` parameter always wins over any matching secret:

```sql
SELECT * FROM read_pdf('encrypted.pdf', password='correct-horse-battery-staple');
```

---

## Skipping unreadable files

By default, any file that cannot be opened or parsed aborts the query with an error. Set `ignore_errors=true` to skip bad files silently instead:

```sql
-- A corrupt or unreadable file aborts the query (default).
SELECT * FROM read_pdf('/data/forms/*.pdf');

-- Bad files are skipped; good files are returned normally.
SELECT * FROM read_pdf('/data/forms/*.pdf', ignore_errors=true);
```

This is useful when scanning a large directory where some files may be corrupted, password-protected with an unknown key, or not actually PDFs despite the `.pdf` extension.

---

## Non-.pdf file extensions

When you name a single file explicitly, the `.pdf` extension is not required:

```sql
-- Accepted: the file is read regardless of extension.
SELECT * FROM read_pdf('/tmp/myform');
```

The `.pdf` filter applies only when expanding a glob pattern or a directory.

---

## Column name handling

Field names come from the `/T` key of each AcroForm field. The extension applies two transformations before using them as column names:

- **Sanitization:** control characters and null bytes are replaced with `_`, and names are capped at 128 characters.
- **Deduplication:** if two fields produce the same column name, or a field collides with the reserved `source_file` column, a numeric suffix is appended (`name`, `name_1`, `name_2`, …).

---

## Building

This repository is based on the [DuckDB extension template](https://github.com/duckdb/extension-template).

### Managing dependencies

Dependencies are managed via [VCPKG](https://vcpkg.io). To set it up:

```shell
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

### Build steps

```sh
make
```

The main binaries built are:

```
./build/release/duckdb                                        # DuckDB shell with extension loaded
./build/release/test/unittest                                 # Test runner with extension linked
./build/release/extension/read_pdf/read_pdf.duckdb_extension  # Loadable extension binary
```

## Running the extension

Start the DuckDB shell with the extension already loaded:

```sh
./build/release/duckdb
```

## Running the tests

```sh
make test
```

Tests live in `./test/sql/` and use DuckDB's SQL test framework.

---

## Setting up CLion

### Opening project

Make sure the DuckDB submodule is available, then open `./duckdb/CMakeLists.txt` as a project in CLion. Go to `Tools -> CMake -> Change Project Root` ([docs](https://www.jetbrains.com/help/clion/change-project-root-directory.html)) and set the root to the top-level directory of this repo.

### Debugging

In `CLion -> Settings -> Build, Execution, Deploy -> CMake`, add the desired build type (Debug, Release, etc.). Set the `build path` to `../build/{build type}` and add the following CMake option:

```
-DDUCKDB_EXTENSION_CONFIGS=<path_to_extension_CMakeLists.txt>
```

To run tests, go to `Run -> Edit Configurations`, add a `CMake Application`, set the target and executable to `unittest`, and add `--test-dir ../../.. [sql]` to the program arguments.
