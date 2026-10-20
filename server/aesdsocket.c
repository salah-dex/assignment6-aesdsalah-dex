#include "aesdsocket.h"
#include "sys/queue.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "aesd_ioctl.h"

#define PORT 9000
#define BACKLOG 5
#define BUFFER_SIZE 1024

#if USE_AESD_CHAR_DEVICE
	#define LOG_FILE "/dev/aesdchar"
/** @brief:
 *  When the string sent over the socket matches the pattern AESDCHAR_IOCSEEKTO:X,Y (where X and Y are unsigned decimal integers)
 *  the application should interpret X as the write command index and Y as the offset within that command.
 */
    #define IOCTL_STRING_CODE "AESDCHAR_IOCSEEKTO:X,Y"

#else
	#define LOG_FILE "/var/tmp/aesdsocketdata"
#endif

#define LOG_FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#define LOG_FILE_FLAGS (O_CREAT | O_WRONLY | O_APPEND)

typedef struct thread_entry {
    pthread_t thread_id;
    int client_socket;
    volatile bool is_active;
    SLIST_ENTRY(thread_entry) next_entry;
} thread_entry_t;

SLIST_HEAD(listhead, thread_entry);
static struct listhead head;

volatile sig_atomic_t exit_flag = 0;
static int server_socket = -1;
static pthread_mutex_t log_file_mutex = PTHREAD_MUTEX_INITIALIZER;


#if  !USE_AESD_CHAR_DEVICE
static timer_t timerid;
static struct sigevent sev;
static struct itimerspec its;
#endif

static int run_as_daemon = 0;

static int setup_server_socket(void);
static int handle_server_loop(void);

static int create_threaded_connection(int client_socket);
static void reap_inactive_threads(void);
static void join_all_threads(void);
static void cleanup(void);
static void signal_handler(int signum);
static void daemonize(void);


static int handle_rx_tx_buffer(int client_socket);
static int process_request(int client_socket,const char *request,size_t request_size,bool is_ioctl,int x_entry_index,int y_entry_offset);
static int send_all(int client_socket, const char *buffer, size_t size);
static bool is_valid_ioctl_string(const char *request, int *x_entry_index, int *y_entry_offset);


#if !USE_AESD_CHAR_DEVICE
static void initialize_timer(void);
#endif

static void parse_arguments(int argc, char *argv[]);
static void *client_handler(void *arg);

#if !USE_AESD_CHAR_DEVICE
void timer_handler(union sigval sv)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[100];

    if (tm_info == NULL) {
        return;
    }

    /* if Device driver is used do not append timestamp */
       strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);
       pthread_mutex_lock(&log_file_mutex);
       FILE *fp = fopen(LOG_FILE, "a");
       if (fp) {
	          fputs(timestamp, fp);
        	  fclose(fp);
          }
    pthread_mutex_unlock(&log_file_mutex);

}
#endif
void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
            close(fd);
        }
    }
}

int main(int argc, char *argv[])
{
    parse_arguments(argc, argv);

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

#if !USE_AESD_CHAR_DEVICE
    unlink(LOG_FILE);
#endif

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server_socket = setup_server_socket();
    if (server_socket < 0) {
        syslog(LOG_ERR, "Failed to set up server socket");
        exit(EXIT_FAILURE);
    }

    if (run_as_daemon) {
        daemonize();
    }
#if !USE_AESD_CHAR_DEVICE
    initialize_timer();
#endif
    SLIST_INIT(&head);
    syslog(LOG_INFO, "Server started on port %d", PORT);

    handle_server_loop();

    cleanup();
    return 0;
}

static void parse_arguments(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            run_as_daemon = 1;
        }
    }
}
#if !USE_AESD_CHAR_DEVICE
static void initialize_timer(void)
{
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_notify_attributes = NULL;

    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        syslog(LOG_ERR, "Failed to create timer");
        exit(EXIT_FAILURE);
    }

    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "Failed to set timer");
        exit(EXIT_FAILURE);
    }
}
#endif

static int setup_server_socket(void)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    int optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, BACKLOG) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static int handle_server_loop(void)
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char client_ip[INET_ADDRSTRLEN];

    while (!exit_flag) {
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            break;
        }

        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        if (create_threaded_connection(client_socket) != 0) {
            syslog(LOG_ERR, "Failed to create thread for client connection");
            close(client_socket);
            continue;
        }

        reap_inactive_threads();
    }

    syslog(LOG_INFO, "Server is shutting down");
    return 0;
}

static int create_threaded_connection(int client_socket)
{
    thread_entry_t *new_entry = malloc(sizeof(thread_entry_t));
    if (new_entry == NULL) {
        syslog(LOG_ERR, "Failed to allocate memory for thread entry: %s", strerror(errno));
        return -1;
    }

    new_entry->client_socket = client_socket;
    new_entry->is_active = true;

    if (pthread_create(&new_entry->thread_id, NULL, client_handler, new_entry) != 0) {
        syslog(LOG_ERR, "Failed to create thread: %s", strerror(errno));
        free(new_entry);
        return -1;
    }

    SLIST_INSERT_HEAD(&head, new_entry, next_entry);
    return 0;
}

static void reap_inactive_threads(void)
{
    thread_entry_t *entry;
  
    SLIST_FOREACH(entry, &head, next_entry) {
        if (!entry->is_active) {
            pthread_join(entry->thread_id, NULL);
            SLIST_REMOVE(&head, entry, thread_entry, next_entry);
            free(entry);
        }
    }
}

static void join_all_threads(void)
{
    while (!SLIST_EMPTY(&head)) {
        thread_entry_t *entry = SLIST_FIRST(&head);
        pthread_join(entry->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, next_entry);
        free(entry);
    }
}

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        exit_flag = 1;
        if (server_socket >= 0) {
            shutdown(server_socket, SHUT_RDWR);
        }
    }
}

static void cleanup(void)
{
    if (server_socket >= 0) {
        close(server_socket);
    }


#if !USE_AESD_CHAR_DEVICE
    if (timerid != (timer_t)0) {
        timer_delete(timerid);
    }
#endif

    join_all_threads();
    pthread_mutex_destroy(&log_file_mutex);
#if !USE_AESD_CHAR_DEVICE
    unlink(LOG_FILE);
#endif
    syslog(LOG_INFO, "Server shutting down");
    closelog();
}

void *client_handler(void *arg)
{
    thread_entry_t *entry = (thread_entry_t *)arg;
    int client_socket = entry->client_socket;

    if (handle_rx_tx_buffer(client_socket) != 0) {
        syslog(LOG_ERR, "Failed to process client data");
    }

    entry->is_active = false;
    close(client_socket);
    return NULL;
}


static int handle_rx_tx_buffer(int client_socket)
{
    char *packet = NULL;
    size_t packet_size = 0;
    size_t packet_capacity = 0;
    char rx_buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    int result = 0;

    while ((bytes_received = recv(client_socket,rx_buffer,sizeof(rx_buffer),0)) > 0)
    {
        for (ssize_t i = 0; i < bytes_received; i++)
        {
            /* Grow the packet buffer when necessary. */
            if (packet_size + 1 >= packet_capacity)
            {
                size_t new_capacity = packet_capacity == 0 ? BUFFER_SIZE : packet_capacity * 2;
                char *new_packet = realloc(packet, new_capacity);

                if (new_packet == NULL)
                {
                    printf("Failed to allocate memory for packet buffer\n");
                    result = -1;
                    break;
                }

                packet = new_packet;
                packet_capacity = new_capacity;
            }

            packet[packet_size++] = rx_buffer[i];
            packet[packet_size] = '\0';

            if (rx_buffer[i] != '\n')
            {
                printf("Received data: %c\n", rx_buffer[i]);
                continue;
            }

            /*
             * A complete newline-terminated request is available.
             * Validate whether it is an ioctl command, then dispatch it.
             */
            int x_entry_index;
            int y_entry_offset;
            bool is_ioctl = is_valid_ioctl_string(packet,&x_entry_index,&y_entry_offset);

            if (process_request(client_socket,packet,packet_size,is_ioctl,x_entry_index,y_entry_offset) < 0)
            {
                result = -1;
                printf("Failed to process request\n");
                break;
            }

            packet_size = 0;
        }

        if (result < 0)
        {
            printf("Error occurred while processing received data\n");
            break;
        }
    }

    if (bytes_received < 0 && errno != EINTR)
    {
        result = -1;
    }

    printf("Cleaning up packet buffer\n");
    free(packet);
    return result;
}


static int send_all(int client_socket, const char *buffer, size_t size)
{
    size_t total_sent = 0;

    while (total_sent < size)
    {
        ssize_t bytes_sent = send(client_socket,
                                  buffer + total_sent,
                                  size - total_sent,
                                  0);

        if (bytes_sent < 0 && errno == EINTR)
        {
            continue;
        }

        if (bytes_sent <= 0)
        {
            return -1;
        }

        total_sent += (size_t)bytes_sent;
    }

    return 0;
}

static int process_request(int client_socket,const char *request,size_t request_size,bool is_ioctl,int x_entry_index,int y_entry_offset)
{
    int device_fd;
    int response_fd;
    int result = 0;

    device_fd = open(LOG_FILE, O_RDWR);
    if (device_fd < 0)
    {
        perror("open " LOG_FILE);
        return -1;
    }

    response_fd = device_fd;

    if (is_ioctl)
    {
        struct aesd_seekto seekto = {
            .write_cmd = x_entry_index,
            .write_cmd_offset = y_entry_offset
        };

        if (ioctl(device_fd, AESDCHAR_IOCSEEKTO, &seekto) < 0)
        {
            perror("ioctl AESDCHAR_IOCSEEKTO");
            result = -1;
        }
        else if (send_all(client_socket, request, request_size) < 0)
        {
            perror("send ioctl request");
            result = -1;
        }
    }
    else
    {
        size_t total_written = 0;

        while (total_written < request_size)
        {
            ssize_t bytes_written = write(device_fd,
                                          request + total_written,
                                          request_size - total_written);

            if (bytes_written < 0 && errno == EINTR)
            {
                continue;
            }

            if (bytes_written <= 0)
            {
                perror("write " LOG_FILE);
                result = -1;
                break;
            }

            total_written += (size_t)bytes_written;
        }

        /*
         * A write advances device_fd's position to the end of the write.
         * Open a separate descriptor to read from the beginning without
         * calling lseek().
         */
        if (result == 0)
        {
            response_fd = open(LOG_FILE, O_RDONLY);

            if (response_fd < 0)
            {
                perror("open " LOG_FILE " for reading");
                result = -1;
            }
        }
    }

    if (result == 0)
    {
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read;

        while ((bytes_read = read(response_fd,
                                  buffer,
                                  sizeof(buffer))) > 0)
        {
            if (send_all(client_socket, buffer, (size_t)bytes_read) < 0)
            {
                perror("send device response");
                result = -1;
                break;
            }
        }

        if (bytes_read < 0)
        {
            if (errno != EINTR)
            {
                perror("read " LOG_FILE);
                result = -1;
            }
        }
    }

    if (response_fd != device_fd)
    {
        if (close(response_fd) < 0)
        {
            perror("close response descriptor");
            result = -1;
        }
    }

    if (close(device_fd) < 0)
    {
        perror("close device descriptor");
        result = -1;
    }

    return result;
}

static bool is_valid_ioctl_string(const char *request, int *x_entry_index, int *y_entry_offset)
{
    const char *prefix = "AESDCHAR_IOCSEEKTO:";
    size_t prefix_len = strlen(prefix);

    if (strncmp(request, prefix, prefix_len) != 0)
    {
        return false;
    }

    const char *params = request + prefix_len;
    if (sscanf(params, "%d,%d", x_entry_index, y_entry_offset) != 2)
    {
        return false;
    }

    return true;
}