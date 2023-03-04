#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * @brief wrapper around recv function
 * @param fd socket with an open connection
 * @param buffer to store data
 * @param size number of bytes to read from the socket
 * @return number of read bytes (>0) if no error, 0 if the connection is closed, <0 in case of error
 */
static inline int my_read(const int sd, char *ret_buffer, const int size) {
    int acc = 0, rcvd = 0;
    while (acc < size) {
        rcvd = recv(sd, &ret_buffer[acc], size - acc, 0);
        if (rcvd <= 0) {
            return rcvd;
        }
        acc += rcvd;
    }
    return acc;
}

int main(int argc, char *args[]) {

    int port = 49200;

    if (argc == 2) {
        port = strtol(args[1], NULL, 10);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Create the socket
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Bind the socket to the specified port number
    if (bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("socket bind failed");
        exit(EXIT_FAILURE);
    }

    // Mark the socket as passive, with a 0 backlog queue
    if (listen(socket_fd, 0) < 0) {
        perror("socket connection listen failed");
        exit(EXIT_FAILURE);
    }

    printf("[MON] Listening on port %d\n", htons(server_addr.sin_port));

    // Accept an incoming connection
    while (1) {
        struct sockaddr_in address;
        int addrlen = sizeof(address);

        printf("[MON] waiting for a connection...\n");
        const int new_socket = accept(socket_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);

        if (new_socket < 0) {
            perror("socket accept failed");
            exit(EXIT_FAILURE);
        }

        printf("[MON] connected with %s:%d\n", inet_ntoa(address.sin_addr), htons(address.sin_port));

        // Get number of consumers
        int consumers = 0;
        if (my_read(new_socket, (char *)&consumers, sizeof(int)) <= 0) {
            perror("socket receive failed, problem while retrieving the number of consumers");
            exit(EXIT_FAILURE);
        }

        /**
         * Allocate an array to receive:
         * [0] => number of items produced by the producer
         * [1] => size of the queue
         * [N] => items consumed by the producer which index ID is N-2
         */
        int monitor_msg[consumers + 2];

        while (1) {
            const int received = my_read(new_socket, (char *)&monitor_msg, sizeof(monitor_msg));

            // Check if any error occurred while receiving
            if (received < 0) {
                printf("[MON] error while receiving messages from %s:%d , closing connection with remote\n", inet_ntoa(address.sin_addr), htons(address.sin_port));
                break;
            }

            // Check if the connection has been closed by the sender
            if (received == 0) {
                printf("[MON] %s:%d closed the connection\n", inet_ntoa(address.sin_addr), htons(address.sin_port));
                break;
            }

            int check = 0;
            int msg_produced = ntohl(monitor_msg[0]);
            int msg_queue = ntohl(monitor_msg[1]);

            check += msg_queue;

            printf("[MON] tot.produced: %d\t in queue: %d\t", msg_produced, msg_queue);

            int tmp = 0;
            for (int i = 2; i < consumers + 2; i++) {
                tmp = ntohl(monitor_msg[i]);
                check += tmp;
                printf("consumed by [%d]:%d\t", i - 2, tmp);
            }
            //printf("check: %d", check);
            printf("\n");
        }

        // Close the connection that the monitor was serving and wait for a new one
        close(new_socket);
    }

    return 0;
}
