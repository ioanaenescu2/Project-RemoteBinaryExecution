#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <libgen.h>

#define PORT 12345

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

void handle_client(int client_sock) {
    printf("Client connected!\n");

    char user[64] = {0};
    ssize_t user_len = recv(client_sock, user, sizeof(user) - 1, 0);
    if (user_len > 0) {
        user[user_len] = '\0';
    } else {
        close(client_sock);
        return;
    }

    while (1) {
        char cmd7[7];
        if (!recv_all(client_sock, cmd7, sizeof(cmd7))) {
            break;
        }
        if (memcmp(cmd7, "SUBMIT\0", 7) != 0) {
            break;
        }

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

        FILE *f = fopen(save_name, "wb");
        if (!f) {
            perror("fopen");
            break;
        }

        long remaining = file_size;
        char buffer[4096];
        while (remaining > 0) {
            size_t chunk = (remaining > (long)sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;
            ssize_t r = recv(client_sock, buffer, chunk, 0);
            if (r <= 0) {
                fclose(f);
                remove(save_name);
                goto done;
            }
            fwrite(buffer, 1, (size_t)r, f);
            remaining -= (long)r;
        }
        fclose(f);

        const char *job_id = "job1";
        send(client_sock, job_id, strlen(job_id) + 1, 0);
    }

done:
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
