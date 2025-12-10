#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>

#define BUFFER_SIZE 4096

int client_sock = -1;

static ssize_t recv_all(int sock, void *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(sock, (char *)buf + total, len - total, 0);
        if (r <= 0) return r; 
        total += (size_t)r;
    }
    return (ssize_t)total;
}

static int recv_cstring(int sock, char *buf, size_t maxlen)
{
    size_t i = 0;
    while (i + 1 < maxlen) {
        char c;
        ssize_t r = recv(sock, &c, 1, 0);
        if (r <= 0) return 0;
        buf[i++] = c;
        if (c == '\0') return 1;
    }
    buf[maxlen - 1] = '\0';
    return 0;
}

static uint32_t sum_file(FILE *f)
{
    if (!f) return 0;
    uint32_t sum = 0;
    unsigned char buffer[BUFFER_SIZE];
    size_t r;
    rewind(f);
    while ((r = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        for (size_t i = 0; i < r; ++i) {
            sum += buffer[i];
        }
    }
    return sum;
}

void handle_connect(char *ip, char *port, char *user)
{
    struct sockaddr_in server_addr;
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(port));
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(client_sock);
        exit(EXIT_FAILURE);
    }

    if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(client_sock);
        exit(EXIT_FAILURE);
    }

    send(client_sock, user, strlen(user), 0);

    printf("Connected to server as %s\n", user);
}

void handle_submit(char *file_path)
{
    if (client_sock < 0) {
        printf("Not connected to server!\n");
        return;
    }

    char cmd[] = "SUBMIT";
    if (send(client_sock, cmd, sizeof(cmd), 0) <= 0) {
        perror("send");
        return;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        perror("fopen");
        return;
    }

    if (send(client_sock, file_path, strlen(file_path) + 1, 0) <= 0) {
        perror("send filename");
        fclose(f);
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (send(client_sock, &file_size, sizeof(file_size), 0) <= 0) {
        perror("send size");
        fclose(f);
        return;
    }

    uint32_t sum = sum_file(f);
    uint32_t sum_net = htonl(sum);
    if (send(client_sock, &sum_net, sizeof(sum_net), 0) <= 0) {
        perror("send checksum");
        fclose(f);
        return;
    }

    rewind(f);
    char buffer[BUFFER_SIZE];
    long sent = 0;
    while (sent < file_size) {
        size_t bytes = fread(buffer, 1, BUFFER_SIZE, f);
        if (bytes <= 0) break;
        if (send(client_sock, buffer, bytes, 0) <= 0) {
            perror("send file");
            break;
        }
        sent += bytes;
    }
    fclose(f);

    char job_id[64] = {0};
    ssize_t r = recv(client_sock, job_id, sizeof(job_id) - 1, 0);
    if (r <= 0) {
        printf("No response from server\n");
        return;
    }
    job_id[r] = '\0';

    if (strncmp(job_id, "ERR", 3) == 0) {
        printf("Server error: %s\n", job_id);
    } else {
        printf("Job submitted! ID: %s\n", job_id);
    }
    
}

void handle_status(char *job_id)
{
    if (client_sock < 0) {
        printf("Not connected to server!\n");
        return;
    }

    const char cmd[] = "STATUS";
    if (send(client_sock, cmd, sizeof(cmd), 0) <= 0) {
        perror("send STATUS");
        return;
    }

    size_t jid_len = strlen(job_id) + 1;
    if (send(client_sock, job_id, jid_len, 0) <= 0) {
        perror("send job_id");
        return;
    }

    char status[64] = {0};
    ssize_t r = recv(client_sock, status, sizeof(status) - 1, 0);
    if (r <= 0) {
        printf("No response from server\n");
        return;
    }
    status[r] = '\0';

    printf("Status for job %s: %s\n", job_id, status);
}

void handle_fetch(char *job_id)
{
    if (client_sock < 0) {
        printf("Not connected to server!\n");
        return;
    }

    const char cmd[] = "FETCH";
    if (send(client_sock, cmd, sizeof(cmd), 0) <= 0) {
        perror("send FETCH");
        return;
    }

    size_t jid_len = strlen(job_id) + 1;
    if (send(client_sock, job_id, jid_len, 0) <= 0) {
        perror("send job_id");
        return;
    }

    char peek[16] = {0};
    ssize_t pr = recv(client_sock, peek, sizeof(peek), MSG_PEEK);
    if (pr > 0 && peek[0] == 'N') {
        char msg[32];
        if (recv_cstring(client_sock, msg, sizeof(msg))) {
            if (strcmp(msg, "NOT_READY") == 0) {
                printf("Job %s is not ready yet.\n", job_id);
                return;
            }
        }
    }

    int exit_code = 0;
    if (recv_all(client_sock, &exit_code, sizeof(exit_code)) <= 0) {
        printf("Failed to receive return code\n");
        return;
    }

    long stdout_len = 0;
    if (recv_all(client_sock, &stdout_len, sizeof(stdout_len)) <= 0) {
        printf("Failed to receive stdout size\n");
        return;
    }
    char *stdout_buf = NULL;
    if (stdout_len > 0) {
        if (stdout_len > 100000000) { printf("Stdout too large\n"); return; }
        stdout_buf = (char *)malloc((size_t)stdout_len + 1);
        if (!stdout_buf) { printf("Memory allocation failed for stdout\n"); return; }
        if (recv_all(client_sock, stdout_buf, (size_t)stdout_len) <= 0) {
            printf("Failed to receive stdout data\n");
            free(stdout_buf);
            return;
        }
        stdout_buf[stdout_len] = '\0';
    }

    long stderr_len = 0;
    if (recv_all(client_sock, &stderr_len, sizeof(stderr_len)) <= 0) {
        printf("Failed to receive stderr size\n");
        free(stdout_buf);
        return;
    }
    char *stderr_buf = NULL;
    if (stderr_len > 0) {
        if (stderr_len > 100000000) { printf("Stderr too large\n"); free(stdout_buf); return; }
        stderr_buf = (char *)malloc((size_t)stderr_len + 1);
        if (!stderr_buf) { printf("Memory allocation failed for stderr\n"); free(stdout_buf); return; }
        if (recv_all(client_sock, stderr_buf, (size_t)stderr_len) <= 0) {
            printf("Failed to receive stderr data\n");
            free(stdout_buf);
            free(stderr_buf);
            return;
        }
        stderr_buf[stderr_len] = '\0';
    }

    printf("Job %s completed. Return code: %d\n", job_id, exit_code);
    printf("--- STDOUT ---\n");
    if (stdout_buf) printf("%s", stdout_buf);
    printf("\n--- STDERR ---\n");
    if (stderr_buf) printf("%s", stderr_buf);
    printf("\n");

    free(stdout_buf);
    free(stderr_buf);
}

void handle_exit()
{
    if (client_sock >= 0) {
        close(client_sock);
        client_sock = -1;
    }
    printf("Disconnected. Goodbye!\n");
    exit(EXIT_SUCCESS);
}

void print_menu()
{
    printf("\nClient commands:\n");
    printf("  connect <IP> <PORT> <USER_NAME>   : Connect to server\n");
    printf("  submit <FILE_PATH>               : Submit a file for execution\n");
    printf("  status <JOB_ID>                  : Check job status\n");
    printf("  fetch <JOB_ID>                   : Fetch job result\n");
    printf("  exit                             : Disconnect and exit\n");
    printf("  help                             : Show this menu\n\n");
}

int main(int argc, char **argv)
{
    char line[512];
    printf("Remote Executor Client \n");
    print_menu();

    while(1)
    {
        printf("> ");
        if((fgets(line, sizeof(line), stdin)) == NULL)
        {
            perror("citire");
            exit(EXIT_FAILURE);
        }

        line[strcspn(line, "\n")]=0;

        char *command=strtok(line, " ");
        if(!command)
        {
            continue;
        }

        if((strcmp(command, "connect")) == 0)
        {
            char *ip=strtok(NULL, " ");
            char *port=strtok(NULL, " ");
            char *user=strtok(NULL, " ");
            if(ip && port && user)
            {
                handle_connect(ip, port, user);
            }
            else
            {
                printf("Use: <IP> <PORT> <USER_NAME> \n");
            }
        }
        else if((strcmp(command, "submit")) == 0)
        {
            char *file_path=strtok(NULL, " ");
            if(file_path)
            {
                handle_submit(file_path);
            }
            else
            {
                printf("Use: <FILE_PATH> \n");
            }
        }
        else if((strcmp(command, "status")) == 0)
        {
            char *job_id=strtok(NULL, " ");
            if(job_id)
            {
                handle_status(job_id);
            }
            else
            {
                printf("Use: <JOB_ID> \n");
            }
        }
        else if((strcmp(command, "fetch")) == 0)
        {
            char *job_id=strtok(NULL, " ");
            if(job_id)
            {
                handle_fetch(job_id);
            }
            else
            {
                printf("Use: <JOB_ID> \n");
            }
        }
        else if((strcmp(command, "exit")) == 0)
        {
            handle_exit();
        }
        else if((strcmp(command, "help")) == 0)
        {
            print_menu();
        }
        else
        {
            printf("Unknown command.\n");
        }
    }

    return(0);
}
