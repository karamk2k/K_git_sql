#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mysql/mysql.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include "mysql_service.h"

#define MAX_LINE_LENGTH 1024
#define MAX_QUERY_LENGTH 2048

// Helper functions (static to this file)
static void create_directory(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0700);
    }
}

static void trim_line(char *str) {
    if (!str) return;
    
    // Trim leading whitespace
    char *start = str;
    while(isspace((unsigned char)*start)) start++;
    if(start != str) {
        memmove(str, start, strlen(start) + 1);
    }

    // Trim trailing whitespace and commas
    if (strlen(str) == 0) return;
    char *end = str + strlen(str) - 1;
    while(end >= str && (isspace((unsigned char)*end) || *end == ',')) end--;
    *(end+1) = '\0';
}

static char* get_column_name(const char *line) {
    const char *start = strchr(line, '`');
    if (!start) return NULL;
    start++; // skip first backtick
    const char *end = strchr(start, '`');
    if (!end) return NULL;
    
    int len = end - start;
    char *name = malloc(len + 1);
    strncpy(name, start, len);
    name[len] = '\0';
    return name;
}

static char** split_lines(const char *str, int *count) {
    if (!str) { *count = 0; return NULL; }
    
    char *s = strdup(str);
    int capacity = 10;
    char **lines = malloc(capacity * sizeof(char*));
    *count = 0;

    char *token = strtok(s, "\n");
    while (token) {
        if (*count >= capacity) {
            capacity *= 2;
            lines = realloc(lines, capacity * sizeof(char*));
        }
        lines[(*count)++] = strdup(token);
        token = strtok(NULL, "\n");
    }
    free(s);
    return lines;
}

static void free_lines(char **lines, int count) {
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

static char* get_table_schema(MYSQL *conn, const char *table_name) {
    char query[MAX_QUERY_LENGTH];
    snprintf(query, sizeof(query), "SHOW CREATE TABLE %s", table_name);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Failed to get schema for table %s: %s\n", table_name, mysql_error(conn));
        return NULL;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) return NULL;

    MYSQL_ROW row = mysql_fetch_row(result);
    char *schema = NULL;
    if (row && row[1]) {
        schema = strdup(row[1]);
    }

    mysql_free_result(result);
    return schema;
}

static void save_sql_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%s", content);
        fclose(fp);
    } else {
        perror("Failed to write SQL file");
    }
}

static void generate_alter_statements(const char *table_name, const char *old_schema, const char *new_schema, char *up_sql, char *down_sql) {
    int old_count = 0, new_count = 0;
    char **old_lines = split_lines(old_schema, &old_count);
    char **new_lines = split_lines(new_schema, &new_count);

    up_sql[0] = '\0';
    down_sql[0] = '\0';

    // Find added lines (in new but not old) -> UP: ADD, DOWN: DROP
    for (int i = 0; i < new_count; i++) {
        char *n_line = strdup(new_lines[i]);
        trim_line(n_line);
        
        // Skip table definition header/footer
        if (strncmp(n_line, "CREATE TABLE", 12) == 0 || strncmp(n_line, ") ENGINE", 8) == 0 || strncmp(n_line, "PRIMARY KEY", 11) == 0 || strncmp(n_line, "KEY", 3) == 0 || strncmp(n_line, "CONSTRAINT", 10) == 0) {
            free(n_line);
            continue;
        }

        int found = 0;
        for (int j = 0; j < old_count; j++) {
            char *o_line = strdup(old_lines[j]);
            trim_line(o_line);
            if (strcmp(n_line, o_line) == 0) {
                found = 1;
            }
            free(o_line);
            if (found) break;
        }

        if (!found) {
            char *col_name = get_column_name(n_line);
            if (col_name) {
                // Simple ADD logic
                char stmt[MAX_LINE_LENGTH];
                snprintf(stmt, sizeof(stmt), "ALTER TABLE `%s` ADD %s;\n", table_name, n_line);
                strcat(up_sql, stmt);

                snprintf(stmt, sizeof(stmt), "ALTER TABLE `%s` DROP COLUMN `%s`;\n", table_name, col_name);
                strcat(down_sql, stmt);
                free(col_name);
            }
        }
        free(n_line);
    }

    // Find removed lines (in old but not new) -> UP: DROP, DOWN: ADD
    for (int i = 0; i < old_count; i++) {
        char *o_line = strdup(old_lines[i]);
        trim_line(o_line);

        if (strncmp(o_line, "CREATE TABLE", 12) == 0 || strncmp(o_line, ") ENGINE", 8) == 0 || strncmp(o_line, "PRIMARY KEY", 11) == 0 || strncmp(o_line, "KEY", 3) == 0 || strncmp(o_line, "CONSTRAINT", 10) == 0) {
            free(o_line);
            continue;
        }

        int found = 0;
        for (int j = 0; j < new_count; j++) {
            char *n_line = strdup(new_lines[j]);
            trim_line(n_line);
            if (strcmp(o_line, n_line) == 0) {
                found = 1;
            }
            free(n_line);
            if (found) break;
        }

        if (!found) {
            char *col_name = get_column_name(o_line);
            if (col_name) {
                char stmt[MAX_LINE_LENGTH];
                snprintf(stmt, sizeof(stmt), "ALTER TABLE `%s` DROP COLUMN `%s`;\n", table_name, col_name);
                strcat(up_sql, stmt);

                snprintf(stmt, sizeof(stmt), "ALTER TABLE `%s` ADD %s;\n", table_name, o_line);
                strcat(down_sql, stmt);
                free(col_name);
            }
        }
        free(o_line);
    }

    free_lines(old_lines, old_count);
    free_lines(new_lines, new_count);
}

static void generate_migrations(const char *start_path, const char *table_name, const char *old_schema, const char *new_schema) {
    char migrations_dir[512];
    snprintf(migrations_dir, sizeof(migrations_dir), "%s/migrations", start_path);
    create_directory(migrations_dir);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d%H%M%S", t);

    char up_path[512];
    char down_path[512];
    snprintf(up_path, sizeof(up_path), "%s/%s_%s_up.sql", migrations_dir, timestamp, table_name);
    snprintf(down_path, sizeof(down_path), "%s/%s_%s_down.sql", migrations_dir, timestamp, table_name);

    if (old_schema) {
        // Generate ALTER statements
        char up_sql[4096];
        char down_sql[4096];
        generate_alter_statements(table_name, old_schema, new_schema, up_sql, down_sql);
        
        // Only save if meaningful changes detected (string is not empty)
        if (strlen(up_sql) > 0 || strlen(down_sql) > 0) {
             save_sql_file(up_path, up_sql);
             save_sql_file(down_path, down_sql);
        } else {
             // Fallback if we detected a change via strcmp but failed to parse diff (e.g. comment/format change only condition?)
             // Or maybe we should just not save if empty.
             // If we are here, save_schema_and_check_diff said strings were different.
             // If up_sql is empty, maybe it was a trivial change we ignored (like ENGINE change line diff?).
             // Let's safe-guard by generating full replacement if ALTER failed?
             // Or just log it.
        }
    } else {
        // Initial creation
        save_sql_file(up_path, new_schema);
        save_sql_file(down_path, "-- Table did not exist previously\nDROP TABLE IF EXISTS table_name;\n");
    }
}

static void save_schema_and_check_diff(const char *table_dir, const char *table_name, const char *schema) {
    char schema_path[512];
    char history_path[512];
    snprintf(schema_path, sizeof(schema_path), "%s/schema.sql", table_dir);
    snprintf(history_path, sizeof(history_path), "%s/history.txt", table_dir);

    // Read existing schema
    char *existing_schema = NULL;
    FILE *fp = fopen(schema_path, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long length = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (length > 0) {
            existing_schema = malloc(length + 1);
            if (fread(existing_schema, 1, length, fp) == (size_t)length) {
                existing_schema[length] = '\0';
            } else {
                free(existing_schema);
                existing_schema = NULL;
            }
        }
        fclose(fp);
    }

    // Compare and save if different
    if (!existing_schema || strcmp(existing_schema, schema) != 0) {
        printf("Change detected in table: %s\n", table_name);

        // Generate migrations
        generate_migrations(table_dir, table_name, existing_schema, schema);

        // Save new schema
        save_sql_file(schema_path, schema);

        // Log to history
        fp = fopen(history_path, "a");
        if (fp) {
            time_t now = time(NULL);
            char *timestamp = ctime(&now);
            timestamp[strcspn(timestamp, "\n")] = 0; // Remove newline
            
            fprintf(fp, "[%s] Schema changed\n", timestamp);
            if (existing_schema) {
                fprintf(fp, "Previous schema was different. Generated ALTER statements.\n");
            } else {
                fprintf(fp, "Initial schema saved.\n");
            }
            fprintf(fp, "----------------------------------------\n");
            fclose(fp);
        }
    }

    if (existing_schema) free(existing_schema);
}

int load_config(DBConfig *config) {
    FILE *file = fopen(".env", "r");
    if (!file) {
        perror("Failed to open .env file");
        return -1;
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        char *delimiter = strchr(line, '=');
        if (delimiter) {
            *delimiter = 0;
            char *key = line;
            char *value = delimiter + 1;
            if (strcmp(key, "DB_HOST") == 0) config->host = strdup(value);
            else if (strcmp(key, "DB_USER") == 0) config->user = strdup(value);
            else if (strcmp(key, "DB_PASS") == 0) config->pass = strdup(value);
            else if (strcmp(key, "DB_NAME") == 0) config->name = strdup(value);
            else if (strcmp(key, "DB_PORT") == 0) config->port = atoi(value);
        }
    }
    fclose(file);
    return 0;
}

void free_config(DBConfig *config) {
    if (config->host) free(config->host);
    if (config->user) free(config->user);
    if (config->pass) free(config->pass);
    if (config->name) free(config->name);
}

MYSQL* connect_db(DBConfig *config) {
    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        fprintf(stderr, "mysql_init() failed\n");
        return NULL;
    }
    if (mysql_real_connect(conn, config->host, config->user, config->pass, config->name, config->port, NULL, 0) == NULL) {
        fprintf(stderr, "Failed to connect to database: %s\n", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }
    return conn;
}

void test_connection(MYSQL *conn) {
    if (mysql_query(conn, "SELECT 1")) {
        fprintf(stderr, "Failed to execute query: %s\n", mysql_error(conn));
        return;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (res) mysql_free_result(res);
    printf("Connection test successful!\n");
}

void close_connection(MYSQL *conn) {
    mysql_close(conn);
}

void track_changes(MYSQL *conn, DBConfig *config) {
    // Ensure 'tables' directory exists
    create_directory("tables");

    char query[MAX_QUERY_LENGTH];
    snprintf(query, sizeof(query), "SELECT table_name FROM information_schema.tables WHERE table_schema = '%s'", config->name);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Failed to fetch tables: %s\n", mysql_error(conn));
        return;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) return;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        char *table_name = row[0];
        
        char table_dir[512];
        snprintf(table_dir, sizeof(table_dir), "tables/%s", table_name);
        create_directory(table_dir);

        char *schema = get_table_schema(conn, table_name);
        if (schema) {
            save_schema_and_check_diff(table_dir, table_name, schema);
            free(schema);
        }
    }

    mysql_free_result(result);
}

void watch_database(DBConfig *config) {
    printf("Starting database watcher for %s...\n", config->name);
    while (1) {
        MYSQL *conn = connect_db(config);
        if (conn) {
            track_changes(conn, config);
            close_connection(conn);
        } else {
            fprintf(stderr, "Retrying connection in 5 seconds...\n");
        }
        sleep(5);
    }
}
