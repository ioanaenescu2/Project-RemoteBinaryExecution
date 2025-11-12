#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

int client_sock = -1;

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
    send(client_sock, cmd, sizeof(cmd), 0);

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        perror("fopen");
        return;
    }

    send(client_sock, file_path, strlen(file_path) + 1, 0);

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    send(client_sock, &file_size, sizeof(file_size), 0);

    char buffer[BUFFER_SIZE];
    long sent = 0;
    while (sent < file_size) {
        size_t bytes = fread(buffer, 1, BUFFER_SIZE, f);
        if (bytes <= 0) break;
        send(client_sock, buffer, bytes, 0);
        sent += bytes;
    }
    fclose(f);

    char job_id[64] = {0};
    recv(client_sock, job_id, sizeof(job_id), 0); 
    printf("Job submitted! ID: %s\n", job_id);
}

void handle_status(char *job_id)
{
    printf("status");
}

void handle_fetch(char *job_id)
{
    printf("fetch");
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

int main(int argc, char **argv)
{
    char line[512];
    printf("Remote Executor Client \n\n");

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
        else
        {
            printf("Unknown command.\n");
        }
    }

    return(0);
}
