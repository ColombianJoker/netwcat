#define _POSIX_C_SOURCE 200112L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFFER_SIZE 32768
#define PROG_NAME "netwcat"

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

// Global flag for graceful termination
volatile sig_atomic_t keep_running = 1;

void graceful_shutdown(int signo) { keep_running = 0; }

// --- Threaded Ring Buffer Structures & Helpers ---

typedef struct {
  char *data;
  size_t capacity;
  size_t head;
  size_t tail;
  size_t count;
  pthread_mutex_t lock;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
  int producer_done;
  int fd_in;
  int fd_out;
  unsigned long long read_limit;
  unsigned long long write_limit;
  unsigned long long total_read;
  unsigned long long total_written;
} RingBuffer;

// Writes to ring buffer handling wrap-around
void rb_write(RingBuffer *rb, const char *data, size_t len) {
  size_t space_to_end = rb->capacity - rb->head;
  if (len <= space_to_end) {
    memcpy(rb->data + rb->head, data, len);
  } else {
    memcpy(rb->data + rb->head, data, space_to_end);
    memcpy(rb->data, data + space_to_end, len - space_to_end);
  }
  rb->head = (rb->head + len) % rb->capacity;
  rb->count += len;
}

// Reads from ring buffer handling wrap-around
void rb_read(RingBuffer *rb, char *data, size_t len) {
  size_t space_to_end = rb->capacity - rb->tail;
  if (len <= space_to_end) {
    memcpy(data, rb->data + rb->tail, len);
  } else {
    memcpy(data, rb->data + rb->tail, space_to_end);
    memcpy(data + space_to_end, rb->data, len - space_to_end);
  }
  rb->tail = (rb->tail + len) % rb->capacity;
  rb->count -= len;
}

// --- Threads ---

void *producer_thread(void *arg) {
  RingBuffer *rb = (RingBuffer *)arg;
  char temp[BUFFER_SIZE];

  while (keep_running) {
    size_t bytes_to_read = BUFFER_SIZE;

    if (rb->read_limit > 0 &&
        (rb->read_limit - rb->total_read) < bytes_to_read) {
      bytes_to_read = rb->read_limit - rb->total_read;
    }
    if (rb->read_limit > 0 && bytes_to_read == 0)
      break;
    if (rb->write_limit > 0 && rb->total_written >= rb->write_limit)
      break;

    ssize_t n_read = read(rb->fd_in, temp, bytes_to_read);
    if (n_read < 0) {
      if (errno == EINTR) {
        if (!keep_running)
          break;
        continue;
      }
      perror(PROG_NAME ": read error");
      break;
    }

    if (n_read == 0) {
      break; // EOF reached (Connection closed or file ended)
    }

    // Push to Ring Buffer
    pthread_mutex_lock(&rb->lock);
    while (rb->capacity - rb->count < (size_t)n_read && keep_running) {
      pthread_cond_wait(&rb->not_full, &rb->lock);
    }
    if (!keep_running) {
      pthread_mutex_unlock(&rb->lock);
      break;
    }

    rb_write(rb, temp, n_read);
    rb->total_read += n_read;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);
  }

  pthread_mutex_lock(&rb->lock);
  rb->producer_done = 1;
  pthread_cond_broadcast(&rb->not_empty);
  pthread_mutex_unlock(&rb->lock);
  return NULL;
}

void *consumer_thread(void *arg) {
  RingBuffer *rb = (RingBuffer *)arg;
  char temp[BUFFER_SIZE];

  while (keep_running) {
    pthread_mutex_lock(&rb->lock);
    while (rb->count == 0 && !rb->producer_done && keep_running) {
      pthread_cond_wait(&rb->not_empty, &rb->lock);
    }
    if (rb->count == 0 && (rb->producer_done || !keep_running)) {
      pthread_mutex_unlock(&rb->lock);
      break;
    }

    size_t to_write = (rb->count < BUFFER_SIZE) ? rb->count : BUFFER_SIZE;
    if (rb->write_limit > 0 &&
        (rb->total_written + to_write) > rb->write_limit) {
      to_write = rb->write_limit - rb->total_written;
    }

    rb_read(rb, temp, to_write);
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->lock);

    // Send TCP packets or write to file
    char *ptr = temp;
    ssize_t remaining = to_write;

    while (remaining > 0 && keep_running) {
      ssize_t n_written = write(rb->fd_out, ptr, remaining);
      if (n_written < 0) {
        if (errno == EINTR) {
          if (!keep_running)
            goto exit_consumer;
          continue;
        }
        if (errno != EPIPE) {
          perror(PROG_NAME ": write error");
        }
        goto exit_consumer;
      }
      if (n_written == 0)
        goto exit_consumer;

      rb->total_written += n_written;
      ptr += n_written;
      remaining -= n_written;
    }

    if (rb->write_limit > 0 && rb->total_written >= rb->write_limit) {
      break;
    }
  }

exit_consumer:
  // If consumer dies early, wake up producer to exit safely
  pthread_mutex_lock(&rb->lock);
  keep_running = 0;
  pthread_cond_broadcast(&rb->not_full);
  pthread_mutex_unlock(&rb->lock);
  return NULL;
}

// --- Main Program ---

void print_usage_and_exit(int status) {
  FILE *stream = (status == EXIT_SUCCESS) ? stdout : stderr;
  fprintf(stream, "Usage:\n");
  fprintf(stream, "  Server mode: " PROG_NAME
                  " -l PORT [-o FILE] [-w WRITELIMIT] [-r READLIMIT] [-v]\n");
  fprintf(stream,
          "  Client mode: " PROG_NAME
          " -c HOST:PORT [-i FILE] [-w WRITELIMIT] [-r READLIMIT] [-v]\n");
  fprintf(stream, "  Options:\n");
  fprintf(stream, "    -l PORT       Listen on local TCP port\n");
  fprintf(stream, "    -c HOST:PORT  Connect to remote HOST and PORT\n");
  fprintf(stream, "    -i INPUT      Read from INPUT (default: stdin)\n");
  fprintf(stream, "    -o OUTPUT     Write to OUTPUT (default: stdout)\n");
  fprintf(stream, "    -r BYTES      Stop reading after LIMIT bytes\n");
  fprintf(stream, "    -w BYTES      Stop writing after LIMIT bytes\n");
  fprintf(stream, "    -v            Verbose mode (show status messages)\n");
  fprintf(stream, "    -h            Show this help message and exit\n");
#ifdef BUILD_TIMESTAMP
  fprintf(stream, "\n  Compiled on: %s. ©️ 2026, Ramón Barrios Láscar.\n",
          BUILD_TIMESTAMP);
#else
  fprintf(stream, "\n  Compiled on: %s %s. ©️ 2026, Ramón Barrios Láscar.\n",
          __DATE__, __TIME__);
#endif
  fprintf(stream, "\n");
  exit(status);
}

int main(int argc, char *argv[]) {
  int opt;
  char *infile = NULL;
  char *outfile = NULL;
  unsigned long long read_limit = 0;
  unsigned long long write_limit = 0;
  char *listen_port = NULL;
  char *connect_hostport = NULL;
  int verbose = 0;

  while ((opt = getopt(argc, argv, "i:o:r:w:l:c:hv")) != -1) {
    switch (opt) {
    case 'i':
      infile = optarg;
      break;
    case 'o':
      outfile = optarg;
      break;
    case 'r':
      read_limit = strtoull(optarg, NULL, 10);
      break;
    case 'w':
      write_limit = strtoull(optarg, NULL, 10);
      break;
    case 'l':
      listen_port = optarg;
      break;
    case 'c':
      connect_hostport = optarg;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'h':
      print_usage_and_exit(EXIT_SUCCESS);
      break;
    default:
      print_usage_and_exit(EXIT_FAILURE);
    }
  }

  if (listen_port && connect_hostport) {
    fprintf(stderr, PROG_NAME ": error: cannot use both -l and -c\n");
    exit(EXIT_FAILURE);
  }
  if (!listen_port && !connect_hostport) {
    fprintf(stderr, PROG_NAME ": error: must specify either -l or -c\n");
    print_usage_and_exit(EXIT_FAILURE);
  }
  if (listen_port && infile) {
    fprintf(stderr, PROG_NAME
            ": error: cannot use -i with -l (server mode only receives)\n");
    exit(EXIT_FAILURE);
  }

  // Gracefully handle termination signals and ignore SIGPIPE for TCP
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  sa.sa_handler = graceful_shutdown;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  int fd_in = -1, fd_out = -1;
  char *display_in = "stdin", *display_out = "stdout";

  // Setup Server Mode
  if (listen_port) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, listen_port, &hints, &res) != 0) {
      fprintf(stderr, PROG_NAME ": error: failed to resolve listen address\n");
      exit(EXIT_FAILURE);
    }

    int server_sock = -1;
    for (p = res; p != NULL; p = p->ai_next) {
      server_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (server_sock < 0)
        continue;

      int optval = 1;
      setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval,
                 sizeof(optval));
      if (p->ai_family == AF_INET6) {
        int no = 0;
        setsockopt(server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
      }
      if (bind(server_sock, p->ai_addr, p->ai_addrlen) == 0)
        break;
      close(server_sock);
    }

    if (p == NULL) {
      fprintf(stderr, PROG_NAME ": error: failed to bind to TCP port %s\n",
              listen_port);
      exit(EXIT_FAILURE);
    }
    int is_ipv6 = (p->ai_family == AF_INET6);
    freeaddrinfo(res);

    if (listen(server_sock, 1) < 0) {
      perror(PROG_NAME ": listen");
      exit(EXIT_FAILURE);
    }

    if (outfile && strcmp(outfile, "-") != 0) {
      fd_out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd_out < 0) {
        perror(PROG_NAME ": open output file");
        exit(EXIT_FAILURE);
      }
      display_out = outfile;
    } else {
      fd_out = STDOUT_FILENO;
    }

    if (verbose) {
      const char *bind_addr = is_ipv6 ? "::" : "0.0.0.0";
      fprintf(stderr, PROG_NAME ": listening on TCP %s:%s and writing to %s\n",
              bind_addr, listen_port, display_out);
    }

    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    fd_in = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (fd_in < 0) {
      perror(PROG_NAME ": accept");
      exit(EXIT_FAILURE);
    }

    if (verbose) {
      char host_str[NI_MAXHOST], port_str[NI_MAXSERV];
      if (getnameinfo((struct sockaddr *)&client_addr, client_len, host_str,
                      sizeof(host_str), port_str, sizeof(port_str),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        fprintf(stderr, PROG_NAME ": connection from %s:%s received\n",
                host_str, port_str);
      }
    }
    close(server_sock); // Close listener, we only handle one client
  }

  // Setup Client Mode
  if (connect_hostport) {
    char *host = strdup(connect_hostport);
    char *port = strchr(host, ':');
    if (!port) {
      fprintf(stderr, PROG_NAME ": error: invalid host:port format\n");
      exit(EXIT_FAILURE);
    }
    *port = '\0';
    port++;

    if (infile && strcmp(infile, "-") != 0) {
      fd_in = open(infile, O_RDONLY);
      if (fd_in < 0) {
        perror(PROG_NAME ": open input file");
        exit(EXIT_FAILURE);
      }
      display_in = infile;
    } else {
      fd_in = STDIN_FILENO;
    }

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
      fprintf(stderr, PROG_NAME ": error: failed to resolve host %s\n", host);
      exit(EXIT_FAILURE);
    }

    for (p = res; p != NULL; p = p->ai_next) {
      fd_out = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd_out < 0)
        continue;
      if (connect(fd_out, p->ai_addr, p->ai_addrlen) == 0)
        break;
      close(fd_out);
      fd_out = -1;
    }

    if (p == NULL) {
      fprintf(stderr, PROG_NAME ": error: failed to connect to %s:%s\n", host,
              port);
      exit(EXIT_FAILURE);
    }
    freeaddrinfo(res);

    if (verbose) {
      fprintf(stderr, PROG_NAME ": connected to TCP %s:%s\n", host, port);
      fprintf(stderr, PROG_NAME ": sending from %s to %s:%s\n", display_in,
              host, port);
    }
    free(host);
  }

  // Initialize Threaded Ring Buffer
  size_t ring_capacity = 1048576; // 1 MB Default
  char *env_cap = getenv("CAT_BUFFER_SIZE");
  if (env_cap) {
    int parsed_cap = atoi(env_cap);
    if (parsed_cap > 0)
      ring_capacity = (size_t)parsed_cap;
  }

  RingBuffer rb;
  memset(&rb, 0, sizeof(RingBuffer));
  rb.capacity = ring_capacity;
  rb.data = malloc(rb.capacity);
  if (!rb.data) {
    fprintf(stderr,
            PROG_NAME ": error: failed to allocate %zu bytes for ring buffer\n",
            rb.capacity);
    exit(EXIT_FAILURE);
  }

  rb.fd_in = fd_in;
  rb.fd_out = fd_out;
  rb.read_limit = read_limit;
  rb.write_limit = write_limit;

  pthread_mutex_init(&rb.lock, NULL);
  pthread_cond_init(&rb.not_empty, NULL);
  pthread_cond_init(&rb.not_full, NULL);

  pthread_t prod_tid, cons_tid;

  // Launching the producer and consumer engines
  pthread_create(&prod_tid, NULL, producer_thread, &rb);
  pthread_create(&cons_tid, NULL, consumer_thread, &rb);

  // Wait for completion
  pthread_join(prod_tid, NULL);
  pthread_join(cons_tid, NULL);

  // Final Output Messages
  if (verbose) {
    if (listen_port) {
      fprintf(stderr,
              PROG_NAME
              ": received from TCP port %s and written %llu bytes to %s\n",
              listen_port, rb.total_written, display_out);
    } else if (connect_hostport) {
      if (read_limit > 0 && rb.total_written < read_limit) {
        fprintf(stderr, PROG_NAME ": %llu received of %llu sent to %s\n",
                rb.total_written, read_limit, connect_hostport);
      } else {
        fprintf(stderr, PROG_NAME ": %llu bytes sent to %s\n", rb.total_written,
                connect_hostport);
      }
    }
  }

  // Cleanup
  if (fd_in != STDIN_FILENO && fd_in >= 0)
    close(fd_in);
  if (fd_out != STDOUT_FILENO && fd_out >= 0)
    close(fd_out);

  pthread_mutex_destroy(&rb.lock);
  pthread_cond_destroy(&rb.not_empty);
  pthread_cond_destroy(&rb.not_full);
  free(rb.data);

  return 0;
}
