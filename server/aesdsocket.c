#include "aesdsocket.h"

/* my application shoud prompt on stdout the action taken */

/* LOCAL DEFINITIONS */
#define PORT 9000
#define BACKLOG 5
#define BUFFER_SIZE 1024
#define LOG_FILE "/var/tmp/aesdsocketdata"
#define LOG_FILE_PERMISSIONS 0666 
#define LOG_FILE_OWNER 1000
#define LOG_FILE_GROUP 1000
#define LOG_FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#define LOG_FILE_FLAGS (O_CREAT | O_WRONLY | O_APPEND)

/* GLOBAL VARIABLES */
volatile sig_atomic_t exit_flag = 0;
int server_socket = -1;
pthread_mutex_t log_file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* private helper functions for handling server operations */
static int setup_server_socket(bool run_as_daemon);
static int handle_server_loop();
static int handle_storage_and_response(int client_socket);
static int append_to_log_file(const char *data, ssize_t data_len);
static int send_log_file_content(int client_socket);
static void cleanup();
static void signal_handler(int signum) ;
static void *client_handler(void *arg) ;

int run_as_daemon = 0;

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        // Parent exits
        exit(EXIT_SUCCESS);
    }

    // Child continues
    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }
   

    // Optional: close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main(int argc, char *argv[]) {

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            run_as_daemon = 1;
        }
    }

    /* 1: open syslog */
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    unlink(LOG_FILE); // Ensure log file is removed on startup

    /* Set up signal handlers for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /*3: Set up server socket */
    server_socket = setup_server_socket(run_as_daemon);

    if (server_socket < 0) {
        syslog(LOG_ERR, "Failed to set up server socket");
        exit(EXIT_FAILURE);
    }

    /* demonize */
    if(run_as_daemon) {
        daemonize();
    }

    syslog(LOG_INFO, "Server started on port %d", PORT);

    /* 4: Main server loop to accept and handle client connections, this never returns unless an error occurs or shutdown is requested */
    handle_server_loop();

    return 0;
}


/* =============== Static functions =============== */
static int handle_server_loop() {
    struct sockaddr_in client_addr; 
    socklen_t client_addr_len = sizeof(client_addr);
     char client_ip[INET_ADDRSTRLEN];
    pthread_t thread_id;
    
    while (!exit_flag) {
        /* 1: Wait for a client to connect */
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, check exit_flag
            }
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            break;
        }
            /* Convert client IP address to string format */
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            /*  Logs message to the syslog “Accepted connection from xxx” where XXXX is the IP address of the connected client.  */
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        /*3: Create a thread to handle the client connection, using the client_handler function */
        if (pthread_create(&thread_id, NULL, client_handler, (void *)(intptr_t)client_socket) != 0) {
            syslog(LOG_ERR, "Failed to create thread: %s", strerror(errno));
            close(client_socket);
            continue;
        }
        pthread_detach(thread_id); // Detach thread to clean up resources when done
    }
    syslog(LOG_INFO,"Closed connection from %s",client_ip);
    cleanup();
    return 0;
}

static void signal_handler(int signum) {
    /* Set the exit flag to indicate graceful shutdown 
     only on SIGINT and SIGTERM */
    if (signum == SIGINT || signum == SIGTERM) {

        exit_flag = 1;

        if (server_socket != -1)
        {
            shutdown(server_socket, SHUT_RDWR);
        }
    }
    
}

static void cleanup() {
    if (server_socket >= 0) {
        close(server_socket);
    }
    pthread_mutex_destroy(&log_file_mutex);
    unlink(LOG_FILE);
    syslog(LOG_INFO, "Server shutting down");
    closelog();
}

/* @brief Handles client connection, receives data, appends to log file, and sends log file content back to client on newline */
/*
should perform the following steps:
a. Receives data from the client in a loop until the connection is closed by the client or an error occurs.
b. Appends the received data to the log file /var/tmp/aesdsocketdata.
c. If a newline character is received in the data, it considers the packet complete and sends the full content of /var/tmp/aesdsocketdata back to the client.
d. Closes the client connection when done.
e. Logs any errors encountered during data reception or file operations to the syslog.
f. cleans up resources before exiting the thread.   
*/


void *client_handler(void *arg) {
    /* 1: Initialize client socket and buffer */
    int client_socket = (intptr_t)arg;

    /* 2: Receive data from the client in a loop until connection is closed or error occurs */
    int result = handle_storage_and_response(client_socket);

    if (result != 0) {
        syslog(LOG_ERR, "Failed to handle storage and response: %s", strerror(errno));
    }

    close(client_socket);
    return NULL;
}

static int setup_server_socket(bool run_as_daemon) {
    /*1: Create socket file descriptor and socket address */
    int sockfd;
    struct sockaddr_in server_addr;
    /* 2: Initialize socket descriptor by calling socket() */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    
    /* 3: Initialize server address structure */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    /* Set socket options to allow reuse of address and port */
    int optval = 1;

    setsockopt(sockfd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &optval,
               sizeof(optval));

    /* 4: Bind the socket to the address and port */
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return -1;
    }
    
    /* 5: Listen for connections */
    if (listen(sockfd, BACKLOG) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static int send_log_file_content(int client_socket) {
    /*1: Lock the log file mutex */
    pthread_mutex_lock(&log_file_mutex);
    /*2: Open the log file for reading */
    int log_fd = open(LOG_FILE, O_RDONLY);
    if (log_fd < 0) {
        syslog(LOG_ERR, "Failed to open log file for reading: %s", strerror(errno));
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }

    char buffer[BUFFER_SIZE];

    ssize_t bytes_read;

    while ((bytes_read = read(log_fd, buffer, sizeof(buffer))) > 0) {

        ssize_t total = 0;

        while (total < bytes_read) {
            ssize_t sent = send(client_socket,buffer + total,bytes_read - total,0);
            if (sent <= 0)
            {
                syslog(LOG_ERR, "Failed to send log file content: %s", strerror(errno));
                close(log_fd);
                pthread_mutex_unlock(&log_file_mutex);
                return -1;
            }
                   

            total += sent;
        }

    }
    close(log_fd);
    pthread_mutex_unlock(&log_file_mutex);
    return 0;
}


static int handle_storage_and_response(int client_socket){

    char *packet = NULL;
    size_t packet_size = 0;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    int newline_found = 0;

    while( !newline_found  && (bytes_received = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
        
        syslog(LOG_INFO, "Received data from client");
        /*1: Dynamically allocate memory for the packet */
        char *new_packet = realloc(packet,packet_size + bytes_received);

        if (new_packet == NULL) {
            syslog(LOG_ERR, "Memory allocation failed: %s", strerror(errno));
            free(packet);
            return -1;
        }
        /* 2: Copy received data to the packet and ensure buffer is null-terminated */
        packet = new_packet;
        
        memcpy(packet + packet_size, buffer, bytes_received);
        packet_size += bytes_received;

        if (memchr(buffer, '\n', bytes_received) != NULL) {
            newline_found = 1;
        }
    }

    if (bytes_received < 0) {
        syslog(LOG_ERR, "Failed to receive data: %s", strerror(errno));
        free(packet);
        return -1;
    }
    /* 3: If a newline character is received, append and send the full content of the log file back to the client */
    if (newline_found) {
        syslog(LOG_INFO, "Newline found, appending and sending back log file content");

        if (append_to_log_file(packet, packet_size) < 0) {
            free(packet);
            return -1;
        }


        if (send_log_file_content(client_socket) < 0) {
            free(packet);
            return -1;
        }
    }
    free(packet);

    return 0;
}

static int append_to_log_file(const char *data, ssize_t data_len)
{
    pthread_mutex_lock(&log_file_mutex);

    int log_fd = open(LOG_FILE, LOG_FILE_FLAGS, LOG_FILE_MODE);

    if (log_fd < 0) {
        syslog(LOG_ERR, "Failed to open log file for appending: %s", strerror(errno));
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }

    if (write(log_fd, data, data_len) < 0) {
        syslog(LOG_ERR, "Failed to write to log file: %s", strerror(errno));
        close(log_fd);
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }

    close(log_fd);
    pthread_mutex_unlock(&log_file_mutex);
    return 0;
}
  