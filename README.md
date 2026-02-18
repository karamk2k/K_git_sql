# MySQL + Git Watcher

## Features

- **Continuous Monitoring**: Runs long-lived watcher threads for Git and MySQL.
- **Git Branch Tracking**: Detects current branch and branch switches.
- **Schema Snapshotting**: Saves normalized schema snapshots in `tables/<table_name>/schema.sql`.
- **Automatic Migrations**:
    - Detects added columns and generates `ALTER TABLE ... ADD ...` statements.
    - Detects removed columns and generates `ALTER TABLE ... DROP ...` statements.
    - Creates timestamped `_up.sql` and `_down.sql` migration files.
- **Branch-Aware SQL Output**:
    - `main` branch: writes baseline snapshots under `dbtables/main/schemas/` and full snapshot file `dbtables/main.sql`.
    - Non-`main` branches: writes delta pairs using timestamp style `dbtables/<branch>/<timestamp>_<table>_up.sql` and `_down.sql`.
- **History Tracking**: Logs schema modifications in `tables/<table_name>/history.txt`.
- **App Logging**: Writes runtime logs to `logs/app.log`.
- **Environment Configuration**: Loads database credentials directly from a `.env` file.

## Prerequisites

- GCC compiler
- MySQL Client libraries (`libmysqlclient-dev`)
- Libgit2 (`libgit2-dev`) - *Linked but currently unused, ready for future git integration.*
- Pthread

## Installation

1.  Clone the repository.
2.  Install dependencies (Ubuntu/Debian):
    ```bash
    sudo apt-get install libmysqlclient-dev libgit2-dev
    ```
3.  Copy the example environment file:
    ```bash
    cp .example.env .env
    ```
4.  Edit `.env` with your database credentials:
    ```
    DB_HOST=127.0.0.1
    DB_USER=root
    DB_PASS=password
    DB_NAME=mydb
    DB_PORT=3306
    ```

## Usage

### Build
To compile the project:
```bash
make
```
This creates the executable at `build/main`.

### Run Foreground
To build and run in foreground:
```bash
make run
```

### Run Background
To run as background process:
```bash
make run-d
```
To stop:
```bash
make stop-d
```

The program connects to the DB, starts Git/MySQL watcher threads, and keeps monitoring.
Output will look like:
```
DB Host: 127.0.0.1
...
Connection test successful!
Starting database watcher for mydb...
Current branch: main
```

### Files Generated
When changes are detected, files are generated like:

```
tables/
└── test_table/
    ├── schema.sql
    ├── history.txt
    └── migrations/
        ├── 20231027100000_test_table_up.sql
        └── 20231027100000_test_table_down.sql

dbtables/
├── main.sql
├── main/
│   └── schemas/
│       └── <table>.sql
└── <non-main-branch>/
    ├── 20231027100000_test_table_up.sql
    └── 20231027100000_test_table_down.sql

logs/
└── app.log
```

## Cleaning Build
To remove build artifacts:
```bash
make clean
```
