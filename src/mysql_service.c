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
#define NAME_SIZE 256

typedef struct EmittedChange {
    char branch[NAME_SIZE];
    char table[NAME_SIZE];
    char *schema;
    struct EmittedChange *next;
} EmittedChange;

static EmittedChange *g_emitted_changes = NULL;

static int has_emitted_change(const char *branch, const char *table, const char *schema) {
    EmittedChange *node = g_emitted_changes;
    while (node) {
        if (strcmp(node->branch, branch) == 0 &&
            strcmp(node->table, table) == 0 &&
            strcmp(node->schema, schema) == 0) {
            return 1;
        }
        node = node->next;
    }
    return 0;
}

static void set_emitted_change(const char *branch, const char *table, const char *schema) {
    EmittedChange *node = g_emitted_changes;
    while (node) {
        if (strcmp(node->branch, branch) == 0 &&
            strcmp(node->table, table) == 0) {
            free(node->schema);
            node->schema = strdup(schema);
            return;
        }
        node = node->next;
    }

    EmittedChange *new_node = malloc(sizeof(EmittedChange));
    if (!new_node) {
        return;
    }
    snprintf(new_node->branch, sizeof(new_node->branch), "%s", branch);
    snprintf(new_node->table, sizeof(new_node->table), "%s", table);
    new_node->schema = strdup(schema);
    new_node->next = g_emitted_changes;
    g_emitted_changes = new_node;
}

static void clear_emitted_change(const char *branch, const char *table) {
    EmittedChange **cursor = &g_emitted_changes;
    while (*cursor) {
        EmittedChange *node = *cursor;
        if (strcmp(node->branch, branch) == 0 &&
            strcmp(node->table, table) == 0) {
            *cursor = node->next;
            free(node->schema);
            free(node);
            return;
        }
        cursor = &((*cursor)->next);
    }
}

static int should_stop(AppContext *ctx) {
    int stop = 0;
    if (!ctx) {
        return 0;
    }
    pthread_mutex_lock(&ctx->lock);
    stop = ctx->stop;
    pthread_mutex_unlock(&ctx->lock);
    return stop;
}

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

static char *normalize_schema(const char *schema) {
    if (!schema) {
        return NULL;
    }

    size_t len = strlen(schema);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }

    size_t i = 0;
    size_t j = 0;
    const char *token = "AUTO_INCREMENT=";
    size_t token_len = strlen(token);

    while (i < len) {
        if (i + token_len <= len && strncmp(&schema[i], token, token_len) == 0) {
            i += token_len;
            while (i < len && isdigit((unsigned char)schema[i])) {
                i++;
            }
            continue;
        }
        out[j++] = schema[i++];
    }

    out[j] = '\0';
    return out;
}

static void sanitize_name(const char *in, char *out, size_t out_size) {
    size_t i = 0;
    if (!in || out_size == 0) {
        return;
    }

    for (; in[i] && i < out_size - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '_' || c == '-' || c == '.') {
            out[i] = (char)c;
        } else {
            out[i] = '_';
        }
    }
    out[i] = '\0';
}

static void write_table_block(FILE *fp, const char *branch, const char *table_name, const char *schema) {
    if (!fp) {
        return;
    }
    fprintf(fp, "-- branch: %s | table: %s\n", branch, table_name);
    fprintf(fp, "%s;\n\n", schema);
}

static char *read_file_content(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (n != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
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

    char *existing_normalized = normalize_schema(existing_schema);
    char *schema_normalized = normalize_schema(schema);

    // Compare and save if different
    if (!existing_normalized || !schema_normalized ||
        strcmp(existing_normalized, schema_normalized) != 0) {
        printf("Change detected in table: %s\n", table_name);

        // Generate migrations
        generate_migrations(table_dir, table_name, existing_normalized, schema_normalized);

        // Save new schema
        if (schema_normalized) {
            save_sql_file(schema_path, schema_normalized);
        }

        // Log to history
        fp = fopen(history_path, "a");
        if (fp) {
            time_t now = time(NULL);
            char *timestamp = ctime(&now);
            timestamp[strcspn(timestamp, "\n")] = 0; // Remove newline
            
            fprintf(fp, "[%s] Schema changed\n", timestamp);
            if (existing_normalized) {
                fprintf(fp, "Previous schema was different. Generated ALTER statements.\n");
            } else {
                fprintf(fp, "Initial schema saved.\n");
            }
            fprintf(fp, "----------------------------------------\n");
            fclose(fp);
        }
    }

    if (existing_schema) free(existing_schema);
    if (existing_normalized) free(existing_normalized);
    if (schema_normalized) free(schema_normalized);
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

void track_changes(MYSQL *conn, DBConfig *config, AppContext *ctx) {
    // Ensure 'tables' directory exists
    create_directory("tables");
    create_directory("dbtables");

    char branch_name[NAME_SIZE];
    char branch_key[NAME_SIZE];
    app_get_branch(ctx, branch_name, sizeof(branch_name));
    sanitize_name(branch_name, branch_key, sizeof(branch_key));
    if (branch_key[0] == '\0') {
        snprintf(branch_key, sizeof(branch_key), "%s", "unknown");
    }
    int is_main_branch = strcmp(branch_key, "main") == 0;

    char branch_dir[512];
    char main_branch_dir[512];
    char main_schemas_dir[512];
    char main_tables_path[512];
    char branch_init_path[512];
    snprintf(branch_dir, sizeof(branch_dir), "dbtables/%s", branch_key);
    snprintf(main_branch_dir, sizeof(main_branch_dir), "dbtables/main");
    snprintf(main_schemas_dir, sizeof(main_schemas_dir), "%s/schemas", main_branch_dir);
    snprintf(main_tables_path, sizeof(main_tables_path), "dbtables/main.sql");
    snprintf(branch_init_path, sizeof(branch_init_path), "%s/.initialized", branch_dir);

    create_directory(branch_dir);
    if (is_main_branch) {
        create_directory(main_branch_dir);
        create_directory(main_schemas_dir);
    }
    int is_branch_bootstrap = is_main_branch && (access(branch_init_path, F_OK) != 0);

    FILE *main_fp = NULL;
    if (is_main_branch) {
        main_fp = fopen(main_tables_path, "w");
    }
    if (main_fp) {
        fprintf(main_fp, "-- all tables snapshot\n");
        fprintf(main_fp, "-- branch: %s\n\n", branch_name);
    }

    char query[MAX_QUERY_LENGTH];
    snprintf(query, sizeof(query), "SELECT table_name FROM information_schema.tables WHERE table_schema = '%s'", config->name);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Failed to fetch tables: %s\n", mysql_error(conn));
        if (main_fp) {
            fclose(main_fp);
        }
        return;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        if (main_fp) {
            fclose(main_fp);
        }
        return;
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        char *table_name = row[0];
        
        char table_dir[512];
        snprintf(table_dir, sizeof(table_dir), "tables/%s", table_name);
        create_directory(table_dir);
        
        char safe_table_name[NAME_SIZE];
        sanitize_name(table_name, safe_table_name, sizeof(safe_table_name));

        char *schema = get_table_schema(conn, table_name);
        if (schema) {
            char *schema_normalized = normalize_schema(schema);

            save_schema_and_check_diff(table_dir, table_name, schema);

            if (schema_normalized) {
                if (is_main_branch) {
                    char main_schema_path[512];
                    snprintf(main_schema_path, sizeof(main_schema_path), "%s/%s.sql", main_schemas_dir, safe_table_name);
                    save_sql_file(main_schema_path, schema_normalized);
                    write_table_block(main_fp, branch_name, table_name, schema_normalized);
                } else {
                    char main_schema_path[512];
                    snprintf(main_schema_path, sizeof(main_schema_path), "%s/%s.sql", main_schemas_dir, safe_table_name);
                    char *main_schema = read_file_content(main_schema_path);
                    char *main_schema_norm = normalize_schema(main_schema);
                    int differs_from_main = (!main_schema_norm) || (strcmp(main_schema_norm, schema_normalized) != 0);

                    if (differs_from_main && !has_emitted_change(branch_key, safe_table_name, schema_normalized)) {
                        char up_sql[8192];
                        char down_sql[1024];
                        const char *reason = "branch_delta";
                        if (!main_schema_norm) {
                            snprintf(up_sql, sizeof(up_sql), "-- table: %s | reason: new_table\n%s;\n\n", table_name, schema_normalized);
                            snprintf(down_sql, sizeof(down_sql), "-- table: %s | reason: rollback_new_table\nDROP TABLE IF EXISTS `%s`;\n\n", table_name, table_name);
                            reason = "new_table";
                        } else {
                            up_sql[0] = '\0';
                            down_sql[0] = '\0';
                            generate_alter_statements(table_name, main_schema_norm, schema_normalized, up_sql, down_sql);
                            reason = "schema_changed";
                        }
                        if (strlen(up_sql) > 0 || strlen(down_sql) > 0) {
                            time_t now = time(NULL);
                            struct tm tm_now;
                            char ts[32];
                            localtime_r(&now, &tm_now);
                            strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", &tm_now);

                            char up_event_path[512];
                            char down_event_path[512];
                            snprintf(up_event_path, sizeof(up_event_path), "%s/%s_%s_up.sql", branch_dir, ts, safe_table_name);
                            snprintf(down_event_path, sizeof(down_event_path), "%s/%s_%s_down.sql", branch_dir, ts, safe_table_name);

                            char up_file_content[8448];
                            char down_file_content[8448];
                            snprintf(up_file_content, sizeof(up_file_content), "-- table: %s | reason: %s\n%s", table_name, reason, up_sql);
                            snprintf(down_file_content, sizeof(down_file_content), "-- table: %s | reason: %s\n%s", table_name, reason, down_sql);

                            save_sql_file(up_event_path, up_file_content);
                            save_sql_file(down_event_path, down_file_content);
                        }
                        set_emitted_change(branch_key, safe_table_name, schema_normalized);
                        app_log(ctx, "MySQL: branch delta for %s -> %s", branch_name, table_name);
                    } else if (!differs_from_main) {
                        clear_emitted_change(branch_key, safe_table_name);
                    }

                    if (main_schema_norm) {
                        free(main_schema_norm);
                    }
                    if (main_schema) {
                        free(main_schema);
                    }
                }

                free(schema_normalized);
            }
            free(schema);
        }
    }

    mysql_free_result(result);
    if (main_fp) {
        fclose(main_fp);
    }
    if (is_main_branch && is_branch_bootstrap) {
        save_sql_file(branch_init_path, "initialized\n");
        app_log(ctx, "MySQL: initialized branch baseline for %s", branch_name);
    }
}

void watch_database(DBConfig *config, AppContext *ctx) {
    printf("Starting database watcher for %s...\n", config->name);
    app_log(ctx, "MySQL: watcher started for database %s", config->name);
    while (!should_stop(ctx)) {
        MYSQL *conn = connect_db(config);
        if (conn) {
            track_changes(conn, config, ctx);
            close_connection(conn);
        } else {
            fprintf(stderr, "Retrying connection in 5 seconds...\n");
        }
        sleep(5);
    }
}
