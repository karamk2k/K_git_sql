#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <pthread.h>
#include <stddef.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;
    char current_branch[256];
} AppContext;

void app_set_branch(AppContext *ctx, const char *branch_name);
void app_get_branch(AppContext *ctx, char *out, size_t out_size);
void app_log(AppContext *ctx, const char *fmt, ...);

#endif // APP_CONTEXT_H
