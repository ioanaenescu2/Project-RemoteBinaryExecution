#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <libgen.h>
#include <stdint.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>

#define PORT 12345
#define MAX_JOBS 256

typedef enum { JOB_RUNNING = 0, JOB_DONE = 1 } JobState;

typedef struct {
    char id[64];
    char user[64];
    char file[300];
    JobState state;
    int exit_code;
    char stdout_path[320];
    char stderr_path[320];
    pid_t pid;
} Job;

static Job jobs[MAX_JOBS];
static int jobs_count = 0;

static Job *find_job_by_id(const char *job_id) {
    for (int i = 0; i < jobs_count; i++) {
        if (strcmp(jobs[i].id, job_id) == 0) return &jobs[i];
    }
    return NULL;
}

static int recv_all(int sock, void *buf, size_t len) {
    size_t off = 0;
    char *p = (char *)buf;

    while (off < len) {
        ssize_t r = recv(sock, p + off, len - off, 0);
        if (r <= 0) {
            return 0;
        }
        off += (size_t)r;
    }
    return 1;
}

static int recv_cstring(int sock, char *buf, size_t maxlen) {
    size_t i = 0;

    while (i + 1 < maxlen) {
        char c;
        ssize_t r = recv(sock, &c, 1, 0);
        if (r <= 0) {
            return 0;
        }
        buf[i++] = c;
        if (c == '\0') {
            return 1;
        }
    }

    buf[maxlen - 1] = '\0';
    return 0;
}

static void *wait_job_thread(void *arg) {
    Job *j = (Job *)arg;
    int status = 0;
    waitpid(j->pid, &status, 0);
    j->state = JOB_DONE;
    j->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return NULL;
}

void handle_client(int client_sock) {
    char user[64] = {0};
    ssize_t user_len = recv(client_sock, user, sizeof(user) - 1, 0);
    if (user_len > 0) {
        user[user_len] = '\0';
        printf("Client connected: %s\n", user);
    } else {
        close(client_sock);
        return;
    }

    static int job_counter = 0;

    while (1) {
        char cmd[16];
        if (!recv_cstring(client_sock, cmd, sizeof(cmd))) {
            break;
        }
        if (strcmp(cmd, "SUBMIT") == 0) {
            printf("Receiving file from client '%s'...\n", user);

            char filename[256];
            if (!recv_cstring(client_sock, filename, sizeof(filename))) {
                break;
            }

            char *base = basename(filename);
            char save_name[300];
            snprintf(save_name, sizeof(save_name), "upload_%s", base);

            long file_size = 0;
            if (!recv_all(client_sock, &file_size, sizeof(file_size))) {
                break;
            }

            uint32_t net_cs = 0;
            if (!recv_all(client_sock, &net_cs, sizeof(net_cs))) {
                break;
            }
            uint32_t expected_cs = ntohl(net_cs);

            FILE *f = fopen(save_name, "wb");
            if (!f) {
                perror("fopen");
                break;
            }

            long remaining = file_size;
            char buffer[4096];
            uint32_t calc_cs = 0;
            while (remaining > 0) {
                size_t chunk = (remaining > (long)sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;
                ssize_t r = recv(client_sock, buffer, chunk, 0);
                if (r <= 0) {
                    fclose(f);
                    remove(save_name);
                    goto done;
                }
                for (ssize_t i = 0; i < r; ++i) {
                    calc_cs += (uint8_t)buffer[i];
                }
                fwrite(buffer, 1, (size_t)r, f);
                remaining -= (long)r;
            }
            fclose(f);

            if (calc_cs != expected_cs) {
                printf("Checksum mismatch for %s (expected %u got %u)\n", save_name, expected_cs, calc_cs);
                const char *err = "BAD_CHECKSUM";
                send(client_sock, err, strlen(err) + 1, 0);
                remove(save_name);
                continue;
            }

            printf("File received successfully: %s (%ld bytes, checksum OK)\n", save_name, file_size);

            job_counter++;
            char job_id[64];
            snprintf(job_id, sizeof(job_id), "job%d", job_counter);

            if (jobs_count < MAX_JOBS) {
                Job *j = &jobs[jobs_count++];
                memset(j, 0, sizeof(*j));
                strncpy(j->id, job_id, sizeof(j->id) - 1);
                strncpy(j->user, user, sizeof(j->user) - 1);
                strncpy(j->file, save_name, sizeof(j->file) - 1);
                snprintf(j->stdout_path, sizeof(j->stdout_path), "%s.out", save_name);
                snprintf(j->stderr_path, sizeof(j->stderr_path), "%s.err", save_name);
                j->state = JOB_RUNNING;
                j->exit_code = -1;

                chmod(j->file, 0755);
                pid_t pid = fork();
                if (pid == 0) {
                    int out_fd = open(j->stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    int err_fd = open(j->stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (out_fd >= 0) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }
                    if (err_fd >= 0) { dup2(err_fd, STDERR_FILENO); close(err_fd); }
                    execl(j->file, j->file, (char *)NULL);
                    perror("exec");
                    _exit(127);
                } else if (pid > 0) {
                    j->pid = pid;
                    pthread_t tid;
                    pthread_create(&tid, NULL, wait_job_thread, j);
                    pthread_detach(tid);
                } else {
                    j->state = JOB_DONE;
                    j->exit_code = -1;
                }
            }

            send(client_sock, job_id, strlen(job_id) + 1, 0);
            continue;
        }
        if (strcmp(cmd, "STATUS") == 0) {
            char job_id[64];
            if (!recv_cstring(client_sock, job_id, sizeof(job_id))) break;
            Job *j = find_job_by_id(job_id);
            const char *resp = j ? (j->state == JOB_DONE ? "DONE" : "RUNNING") : "NOT_FOUND";
            send(client_sock, resp, strlen(resp) + 1, 0);
            continue;
        }
        if (strcmp(cmd, "FETCH") == 0) {
            char job_id[64];
            if (!recv_cstring(client_sock, job_id, sizeof(job_id))) break;
            Job *j = find_job_by_id(job_id);
            if (!j || j->state != JOB_DONE) {
                const char *resp = "NOT_READY";
                send(client_sock, resp, strlen(resp) + 1, 0);
                continue;
            }
            int exit_code = j->exit_code;
            send(client_sock, &exit_code, sizeof(exit_code), 0);
            FILE *o = fopen(j->stdout_path, "rb");
            long olen = 0; if (o) { fseek(o, 0, SEEK_END); olen = ftell(o); fseek(o, 0, SEEK_SET); }
            send(client_sock, &olen, sizeof(olen), 0);
            if (o) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), o)) > 0) { send(client_sock, buf, n, 0); } fclose(o); }
            FILE *e = fopen(j->stderr_path, "rb");
            long elen = 0; if (e) { fseek(e, 0, SEEK_END); elen = ftell(e); fseek(e, 0, SEEK_SET); }
            send(client_sock, &elen, sizeof(elen), 0);
            if (e) { char buf2[4096]; size_t n2; while ((n2 = fread(buf2, 1, sizeof(buf2), e)) > 0) { send(client_sock, buf2, n2, 0); } fclose(e); }
            continue;
        }
        break;
    }

done:
    printf("Client disconnected: %s\n", user);
    close(client_sock);
}

int main() {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_sock, 5) < 0) {
        perror("listen");
        return 1;
    }

    printf("Server listening on port %d...\n", PORT);

    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock >= 0) {
            handle_client(client_sock);
        }
    }

    return 0;
}
