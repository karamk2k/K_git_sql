#ifndef MYSQL_SERVICE_H
#define MYSQL_SERVICE_H

#include <mysql/mysql.h>
#include <time.h>
#include <pthread.h>

typedef struct {
    char *host;
    char *user;
    char *pass;
    char *name;
    int port;
} DBConfig;


typedef struct {
    char *table;
    char *column;
    char *type;
    char *action;
    time_t timestamp;
} Change;

typedef struct {
    char *name;
    Change *changes;
    int changes_count;
} Table;


typedef struct {
  char **fields;
  int fields_count;
} Fields;

int load_config(DBConfig *config);
void free_config(DBConfig *config);
MYSQL* connect_db(DBConfig *config);
void test_connection(MYSQL *conn);
void close_connection(MYSQL *conn);
void track_changes(MYSQL *conn, DBConfig *config);
void watch_database(DBConfig *config);
#endif // MYSQL_SERVICE_H
