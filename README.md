# MySQL Schema Watcher

A C-based utility that monitors a MySQL database for schema changes and automatically generates migration files (up/down) and tracks history.

## Features

- **Continuous Monitoring**: Runs as a daemon/watcher process to check for schema changes every minute .
- **Schema Snapshotting**: Saves the current `CREATE TABLE` schema for all tables in `tables/<table_name>/schema.sql`.
- **Automatic Migrations**:
    - Detects added columns and generates `ALTER TABLE ... ADD ...` statements.
    - Detects removed columns and generates `ALTER TABLE ... DROP ...` statements.
    - Creates timestamped `_up.sql` and `_down.sql` migration files.
- **History Tracking**: Logs all modifications in `tables/<table_name>/history.txt`.
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

### Run
To build and run the watcher:
```bash
make run
```

The program will start, connect to the database, and enter a loop watching for changes.
Output will look like:
```
DB Host: 127.0.0.1
...
Connection test successful!
Starting database watcher for mydb...
```

### Files Generated
When a change is detected (e.g., adding a column to `test_table`), the following structure is created:

```
tables/
└── test_table/
    ├── schema.sql              # Current full schema
    ├── history.txt             # Log of changes
    └── migrations/
        ├── 20231027100000_test_table_up.sql   # ALTER TABLE ... ADD column ...
        └── 20231027100000_test_table_down.sql # ALTER TABLE ... DROP column ...
```

## Cleaning Build
To remove build artifacts:
```bash
make clean
```
