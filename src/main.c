#include <stdio.h>
#include <pthread.h>
#include <mysql/mysql.h>
#include "mysql_service.h"
#include "git_service.h"
#include "app_context.h"

typedef struct {
    DBConfig *config;
    AppContext *ctx;
} ServiceArgs;

static void *git_thread_main(void *arg) {
    ServiceArgs *args = (ServiceArgs *)arg;
    git_init(args->ctx);
    return NULL;
}

static void *mysql_thread_main(void *arg) {
    ServiceArgs *args = (ServiceArgs *)arg;
    watch_database(args->config, args->ctx);
    return NULL;
}

int main() {
    DBConfig config = {0};
    AppContext app_ctx = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .stop = 0,
        .current_branch = "unknown"
    };
    pthread_t git_thread;
    pthread_t mysql_thread;
    ServiceArgs service_args = {
        .config = &config,
        .ctx = &app_ctx
    };

    if (load_config(&config) != 0) {
        printf("Failed to load config\n");
        return 1;
    }
    
    printf("DB Host: %s\n", config.host);
    printf("DB User: %s\n", config.user);
    printf("DB Port: %d\n", config.port);

    if (pthread_create(&git_thread, NULL, git_thread_main, &service_args) != 0) {
        fprintf(stderr, "Failed to start git thread\n");
        free_config(&config);
        return 1;
    }

    MYSQL *conn = connect_db(&config);
    if (!conn) {
        pthread_mutex_lock(&app_ctx.lock);
        app_ctx.stop = 1;
        pthread_cond_broadcast(&app_ctx.cond);
        pthread_mutex_unlock(&app_ctx.lock);
        pthread_cancel(git_thread);
        pthread_join(git_thread, NULL);
        free_config(&config);
        return 1;
    }
    test_connection(conn);
    close_connection(conn);

    if (pthread_create(&mysql_thread, NULL, mysql_thread_main, &service_args) != 0) {
        fprintf(stderr, "Failed to start mysql thread\n");
        pthread_cancel(git_thread);
        pthread_join(git_thread, NULL);
        free_config(&config);
        return 1;
    }

    pthread_join(git_thread, NULL);
    pthread_join(mysql_thread, NULL);

    free_config(&config);
    return 0;
}
