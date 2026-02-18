#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "app_context.h"

static void ensure_logs_dir(void)
{
    struct stat st = {0};
    if (stat("logs", &st) == -1) {
        mkdir("logs", 0700);
    }
}

void app_set_branch(AppContext *ctx, const char *branch_name)
{
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    snprintf(ctx->current_branch, sizeof(ctx->current_branch), "%s",
             branch_name ? branch_name : "unknown");
    pthread_mutex_unlock(&ctx->lock);
}

void app_get_branch(AppContext *ctx, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (!ctx) {
        snprintf(out, out_size, "%s", "unknown");
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    snprintf(out, out_size, "%s",
             ctx->current_branch[0] ? ctx->current_branch : "unknown");
    pthread_mutex_unlock(&ctx->lock);
}

void app_log(AppContext *ctx, const char *fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    ensure_logs_dir();

    if (ctx) {
        pthread_mutex_lock(&ctx->lock);
    }

    FILE *fp = fopen("logs/app.log", "a");
    if (fp) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
        fprintf(fp, "[%s] %s\n", ts, message);
        fclose(fp);
    }

    if (ctx) {
        pthread_mutex_unlock(&ctx->lock);
    }
}
