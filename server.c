
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define PORT 5000

volatile sig_atomic_t was_sighup = 0;
volatile sig_atomic_t stop_server = 0;

void sighup_handler(int signal_number)
{
    if (signal_number == SIGHUP)
        was_sighup = 1;

    if (signal_number == SIGINT)
        stop_server = 1;
}

int main(void)
{
    int server_fd = -1;
    int client_fd = -1;
    int status = EXIT_FAILURE;
    int mask_saved = 0;

    sigset_t blocked_mask;
    sigset_t original_mask;
    sigset_t wait_mask;

    sigemptyset(&blocked_mask);
    sigaddset(&blocked_mask, SIGHUP);
    sigaddset(&blocked_mask, SIGINT);

    if (sigprocmask(SIG_BLOCK, &blocked_mask, &original_mask) == -1) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    mask_saved = 1;

    struct sigaction action;
    memset(&action, 0, sizeof(action));

    action.sa_handler = sighup_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGHUP, &action, NULL) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1) {
        perror("sigaction");
        goto cleanup;
    }

    wait_mask = original_mask;
    sigdelset(&wait_mask, SIGHUP);
    sigdelset(&wait_mask, SIGINT);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        goto cleanup;
    }

    if (server_fd >= FD_SETSIZE) {
        fprintf(stderr, "Invalid server descriptor\n");
        goto cleanup;
    }

    int reuse = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
        &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt");
        goto cleanup;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address,
        sizeof(address)) == -1) {
        perror("bind");
        goto cleanup;
    }

    if (listen(server_fd, 1) == -1) {
        perror("listen");
        goto cleanup;
    }

    printf("Server started on port %d\n", PORT);
    fflush(stdout);

    while (1) {
        fd_set read_fds;

        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        int max_fd = server_fd;
        int watched_client = client_fd;

        if (watched_client != -1) {
            FD_SET(watched_client, &read_fds);

            if (watched_client > max_fd)
                max_fd = watched_client;
        }

        if (was_sighup) {
            was_sighup = 0;
            printf("Received SIGHUP\n");
            fflush(stdout);
        }

        if (stop_server) {
            status = EXIT_SUCCESS;
            break;
        }

        int result = pselect(
            max_fd + 1,
            &read_fds,
            NULL,
            NULL,
            NULL,
            &wait_mask
        );

        int saved_errno = errno;

        if (was_sighup) {
            was_sighup = 0;
            printf("Received SIGHUP\n");
            fflush(stdout);
        }

        if (stop_server) {
            status = EXIT_SUCCESS;
            break;
        }

        if (result == -1) {
            if (saved_errno == EINTR)
                continue;

            errno = saved_errno;
            perror("pselect");
            goto cleanup;
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            int new_fd = accept(server_fd, NULL, NULL);

            if (new_fd == -1) {
                if (errno == EINTR)
                    continue;

                perror("accept");
                goto cleanup;
            }

            printf("New connection\n");

            if (client_fd == -1 && new_fd < FD_SETSIZE) {
                client_fd = new_fd;
                printf("Client accepted\n");
            }
            else {
                printf("Extra connection closed\n");
                close(new_fd);
            }

            fflush(stdout);
        }

        if (watched_client != -1 &&
            FD_ISSET(watched_client, &read_fds)) {
            char buffer[1024];

            ssize_t received = recv(
                watched_client,
                buffer,
                sizeof(buffer),
                0
            );

            if (received > 0) {
                printf("Received data: %zd bytes\n", received);
                fflush(stdout);
            }
            else if (received == 0) {
                printf("Client disconnected\n");
                close(watched_client);
                client_fd = -1;
            }
            else if (errno != EINTR) {
                perror("recv");
                close(watched_client);
                client_fd = -1;
            }
        }
    }

cleanup:

    if (client_fd != -1)
        close(client_fd);

    if (server_fd != -1)
        close(server_fd);

    if (mask_saved) {
        if (sigprocmask(SIG_SETMASK, &original_mask, NULL) == -1) {
            perror("sigprocmask");
            status = EXIT_FAILURE;
        }
    }

    printf("Server stopped\n");

    return status;
}