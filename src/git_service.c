#include <git2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app_context.h"

#define BRANCH_NAME_SIZE 256

static void log_git_error(const char *action, int error_code)
{
    const git_error *error = git_error_last();
    fprintf(stderr, "%s failed (%d): %s\n",
            action,
            error_code,
            (error && error->message) ? error->message : "unknown error");
}

static int open_repository(git_repository **repo)
{
    const char *path = getenv("PWD");
    if (!path) {
        path = ".";
    }

    int rc = git_repository_open(repo, path);
    if (rc != 0) {
        log_git_error("git_repository_open", rc);
        return -1;
    }
    return 0;
}

static int resolve_head_oid(git_repository *repo, git_oid *oid)
{
    int rc = git_reference_name_to_id(oid, repo, "HEAD");
    if (rc != 0) {
        log_git_error("git_reference_name_to_id(HEAD)", rc);
        return -1;
    }
    return 0;
}

static int should_stop(AppContext *ctx)
{
    int stop = 0;
    if (!ctx) {
        return 0;
    }

    pthread_mutex_lock(&ctx->lock);
    stop = ctx->stop;
    pthread_mutex_unlock(&ctx->lock);
    return stop;
}

static int get_current_branch_name(git_repository *repo, char *branch_name, size_t branch_name_size)
{
    git_reference *head = NULL;
    int rc = git_repository_head(&head, repo);
    if (rc != 0) {
        log_git_error("git_repository_head", rc);
        return -1;
    }

    if (git_reference_is_branch(head)) {
        const char *name = NULL;
        rc = git_branch_name(&name, head);
        if (rc != 0) {
            log_git_error("git_branch_name", rc);
            git_reference_free(head);
            return -1;
        }
        snprintf(branch_name, branch_name_size, "%s", name ? name : "unknown");
    } else {
        snprintf(branch_name, branch_name_size, "%s", "DETACHED_HEAD");
    }

    git_reference_free(head);
    return 0;
}

static void print_commit_info(git_repository *repo, const git_oid *commit_oid)
{
    git_commit *commit = NULL;
    int rc = git_commit_lookup(&commit, repo, commit_oid);
    if (rc != 0) {
        log_git_error("git_commit_lookup", rc);
        return;
    }

    const char *message = git_commit_message(commit);
    const git_signature *author = git_commit_author(commit);
    char commit_id[GIT_OID_HEXSZ + 1];
    git_oid_tostr(commit_id, sizeof(commit_id), commit_oid);

    printf("New commit detected!\n");
    printf("Author: %s\n", author ? author->name : "unknown");
    printf("Message: %s\n", message ? message : "(no message)");
    printf("Commit: %s\n", commit_id);

    git_commit_free(commit);
}

static void watch_repo_changes(git_repository *repo, git_oid *last_oid, char *last_branch_name, size_t branch_name_size, AppContext *ctx)
{
    git_oid current_oid;
    char current_branch_name[BRANCH_NAME_SIZE];

    while (!should_stop(ctx)) {
        if (resolve_head_oid(repo, &current_oid) == 0 &&
            git_oid_cmp(last_oid, &current_oid) != 0) {
            *last_oid = current_oid;
            print_commit_info(repo, &current_oid);
            app_log(ctx, "Git: new commit detected on branch %s", last_branch_name);
        }

        if (get_current_branch_name(repo, current_branch_name, sizeof(current_branch_name)) == 0 &&
            strcmp(last_branch_name, current_branch_name) != 0) {
            printf("Branch changed: %s -> %s\n", last_branch_name, current_branch_name);
            app_log(ctx, "Git: branch changed %s -> %s", last_branch_name, current_branch_name);
            app_set_branch(ctx, current_branch_name);
            snprintf(last_branch_name, branch_name_size, "%s", current_branch_name);
        }

        sleep(2);
    }
}

void git_init(AppContext *ctx)
{
    git_repository *repo = NULL;
    git_oid last_oid;
    char last_branch_name[BRANCH_NAME_SIZE];
    int rc = git_libgit2_init();

    if (rc < 0) {
        log_git_error("git_libgit2_init", rc);
        return;
    }

    if (open_repository(&repo) != 0) {
        git_libgit2_shutdown();
        return;
    }

    if (resolve_head_oid(repo, &last_oid) != 0) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        return;
    }

    if (get_current_branch_name(repo, last_branch_name, sizeof(last_branch_name)) != 0) {
        snprintf(last_branch_name, sizeof(last_branch_name), "%s", "unknown");
    }
    printf("Current branch: %s\n", last_branch_name);
    app_set_branch(ctx, last_branch_name);
    app_log(ctx, "Git: watcher started on branch %s", last_branch_name);

    watch_repo_changes(repo, &last_oid, last_branch_name, sizeof(last_branch_name), ctx);

    git_repository_free(repo);
    git_libgit2_shutdown();
}
