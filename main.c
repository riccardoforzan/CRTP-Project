#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define PRODUCER_MAX_WAIT 200000000  // Upper bound to sleep time for a consumer (0.2s)
#define CONSUMER_MAX_WAIT 1000000000 // Upper bound to sleep time for a consumer (1s)

/**
 * @brief struct used to setup the remote connection with the monitoring process
 * This struct is used to pass data of heterogeneous time to the routine that would
 * be executed by the monitor thread created by the main
 */
struct monitor_data {
    int interval;
    int consumers;
    struct sockaddr_in monitor_addr;
};

// Shared variables that should not change value
int messages;
int buffer_size;

// Inter-process communication variables
pthread_mutex_t mutex;
pthread_cond_t space_in_buffer, data_in_buffer;
int *buffer;
int write_index = 0; // Next free slot in the buffer
int read_index = 0;  // Next slot to be read by a consumer
int produced = 0;    // Number of messages produced
int *consumed;       // Array of integers keeping track of the number of messages consumed by each consumer
char finished = 0;   // Set to a value different from zero when the producer has produced the specified number of messages

/**
 * @brief Sleeps the current thread for an amount of nanoseconds in the range [0 - bound)
 * @param bound Upper bound on the sleep time
 */
static inline void bounded_nanosleep(const long bound) {
    static struct timespec time;
    time.tv_sec = 0;
    time.tv_nsec = rand() % bound;
    nanosleep(&time, NULL);
}

/**
 * @brief Produces a message and stores them it in the shared buffer (util full).
 * After producing the message, signals to consumer threads the presence of a new message.
 */
void *producer() {

    for (int i = 0; i < messages; i++) {

        bounded_nanosleep(PRODUCER_MAX_WAIT); // Simulate a complex routine for message production

        pthread_mutex_lock(&mutex);
        while ((write_index + 1) % buffer_size == read_index) {
            // Unlock the mutex and wait on the condition atomically
            pthread_cond_wait(&space_in_buffer, &mutex);
        }

        buffer[write_index] = i; // Store the message in the butter
        write_index = (write_index + 1) % buffer_size;
        produced += 1;

        pthread_cond_signal(&data_in_buffer); // Inform consumers there is a new message
        pthread_mutex_unlock(&mutex);
    }

    // Broadcast all consumers that the producer has finished producing, no more messages will ever be produced
    pthread_mutex_lock(&mutex);
    finished = 1;
    pthread_cond_broadcast(&data_in_buffer);
    pthread_mutex_unlock(&mutex);

    return NULL;
}

/**
 * @brief Consumes a message from the buffer (if any), otherwise waits until a new message is generated.
 * After consuming a message, signals to the producer thread that there is space available in the buffer.
 * @param arg it's passed as a void pointer, a pointer to int that would be used to identify the consumer
 * in the output messages
 */
void *consumer(void *arg) {

    const int id = *((int *)arg); // Retrieve the ID passed as argument by casting the pointer
    free(arg);

    int item;
    while (1) {
        pthread_mutex_lock(&mutex);

        // Check if the producer has finished producing, no more messages will ever arrive
        if (finished && read_index == write_index) {
            pthread_mutex_unlock(&mutex);
            break; // exit the while(1) loop
        }

        // Wait for a new message
        while (!finished && read_index == write_index) {
            // Unlock the mutex and wait on the condition atomically
            pthread_cond_wait(&data_in_buffer, &mutex);
        }

        item = buffer[read_index];
        read_index = (read_index + 1) % buffer_size; // Shift the read index
        consumed[id] += 1;
        pthread_cond_signal(&space_in_buffer);
        pthread_mutex_unlock(&mutex);

        // Simulate complex operation
        bounded_nanosleep(CONSUMER_MAX_WAIT);
    }

    return NULL;
}

/**
 * @brief Reads the length of the buffer at a given interval of time and sends it to a monitor server.
 * @param arg pointer to monitor_data struct
 */
static void *monitor(void *arg) {

    const struct monitor_data *data = (struct monitor_data *)arg;
    const int interval = data->interval;
    const int consumers = data->consumers;
    const struct sockaddr_in monitor_addr = data->monitor_addr;

    // Open a TCP socket to the monitor serve
    const int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        perror("[MONITOR]: socket creation failed");
        exit(EXIT_FAILURE);
    }

    if (connect(sockfd, (struct sockaddr *)&monitor_addr, sizeof(monitor_addr)) < 0) {
        perror("[MONITOR]: socket connection failed");
        exit(EXIT_FAILURE);
    }

    // Send the number of consumers to the monitor server
    if (send(sockfd, &consumers, sizeof(consumers), 0) < 0) {
        perror("[MONITOR]: number of consumers send failed");
        exit(EXIT_FAILURE);
    }

    while (1) {

        pthread_mutex_lock(&mutex);

        const int queue_length = (write_index - read_index + buffer_size) % buffer_size;

        // Check if the message that it's in preparation is the last message to be sent
        char last_message = finished && (queue_length == 0);

        // Prepare data for the monitor
        int monitor_msg[consumers + 2];       // Array of integer messages to send
        monitor_msg[0] = htonl(produced);     // Number of produced messages so far
        monitor_msg[1] = htonl(queue_length); // Queue length

        for (int i = 0; i < consumers; i++) {
            monitor_msg[i + 2] = htonl(consumed[i]);
        }

        pthread_mutex_unlock(&mutex);

        // Send the message to the monitor server
        if (send(sockfd, &monitor_msg, sizeof(monitor_msg), 0) < 0) {
            perror("[MONITOR]: data send failed");
            exit(EXIT_FAILURE);
        }

        // If last message then exit the loop and do not send messages anymore
        if (last_message) {
            break;
        }

        // Wait for the next sample time
        sleep(interval);
    }

    // Close the socket with the monitor server
    close(sockfd);

    return NULL;
}

int main(int argc, char *args[]) {

    if (argc < 6) {
        printf("%s <IP:char*>:<PORT:int> <INTERVAL:int [s]> <CONSUMERS:int> <# BUFFER SIZE:int> <# MESSAGES:int> \n", args[0]);
        return 0;
    }

    // Get data to remote connect to the monitor
    char remote_ip[16];
    int remote_port;
    sscanf(args[1], "%255[^:]:%d", remote_ip, &remote_port);

    // Get sampling interval
    const int interval = strtol(args[2], NULL, 10);

    // Get number of consumers
    const int consumers = (int)strtol(args[3], NULL, 10);

    // Allocate buffer of given size
    buffer_size = (int)strtol(args[4], NULL, 10);
    buffer = malloc(sizeof(int) * buffer_size);
    if (buffer == NULL) {
        perror("[MAIN] buffer malloc failed");
        exit(EXIT_FAILURE);
    }
    printf("[MAIN] allocated buffer for %d elements\n", buffer_size);

    // Get number of messages to produce
    messages = (int)strtol(args[5], NULL, 10);
    printf("[MAIN] set production of %d elements\n", messages);

    // Initialize mutex and condition variables
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&space_in_buffer, NULL);
    pthread_cond_init(&data_in_buffer, NULL);

    // Create producer thread
    printf("[MAIN] starting producer\n");
    pthread_t producer_thread;
    pthread_create(&producer_thread, NULL, producer, NULL);

    // Consumer counter
    consumed = malloc(sizeof(int) * consumers);
    if (consumed == NULL) {
        perror("[MAIN] consumed malloc failed");
        exit(EXIT_FAILURE);
    }

    // Create consumers threads
    printf("[MAIN] creating %d consumer\n", consumers);
    pthread_t threads[consumers];
    for (int i = 0; i < consumers; i++) {

        // Allocate the ID variable on the heap
        int *id = malloc(sizeof(*id));
        if (id == NULL) {
            perror("[MAIN] id malloc failed");
            exit(EXIT_FAILURE);
        }

        // Set the ID of the consumer as the index of the loop
        *id = i;

        printf("[MAIN] consumer %d thread created\n", i);
        pthread_create(&threads[i], NULL, consumer, id);
    }

    // Set up monitor connection parameters
    struct sockaddr_in monitor_addr;
    monitor_addr.sin_family = AF_INET;
    monitor_addr.sin_port = htons(remote_port);
    monitor_addr.sin_addr.s_addr = inet_addr(remote_ip);

    // Send data to monitor
    struct monitor_data setup_data;
    setup_data.interval = interval;
    setup_data.consumers = consumers;
    setup_data.monitor_addr = monitor_addr;

    printf("[MAIN] starting monitor thread\n");
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, monitor, &setup_data);

    // Wait for all consumer threads to finish
    for (int i = 0; i < consumers + 1; i++) {
        pthread_join(threads[i], NULL);
    }

    // Wait also termination of monitor thread
    pthread_join(monitor_thread, NULL);

    printf("[MAIN] consumed all messages\n");
    
    free(buffer);
    free(consumed);

    return 0;
}