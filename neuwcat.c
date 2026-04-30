#define _POSIX_C_SOURCE 200112L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Safe datagram size to avoid EMSGSIZE on strict operating systems
#define BUFFER_SIZE 8192
#define PROG_NAME "neuwcat"

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

// Global flag for graceful termination
volatile sig_atomic_t keep_running = 1;

void graceful_shutdown(int signo) { keep_running = 0; }

void print_usage_and_exit(int status) {
  FILE *stream = (status == EXIT_SUCCESS) ? stdout : stderr;
  fprintf(stream, "Usage:\n");
  fprintf(stream,
          "  Server mode: " PROG_NAME " -l PORT [-o FILE] [-w WRITELIMIT] "
          "[-r READLIMIT] [-v]\n");
  fprintf(stream, "  Client mode: " PROG_NAME " -c HOST:PORT [-i FILE] [-w "
                  "WRITELIMIT] [-r READLIMIT] [-v]\n");
  fprintf(stream, "  Options:\n");
  fprintf(stream, "    -l PORT       Listen on local UDP port\n");
  fprintf(stream, "    -c HOST:PORT  Send to remote HOST and UDP PORT\n");
  fprintf(stream, "    -i INPUT      Read from INPUTFILE (default: stdin)\n");
  fprintf(stream, "    -o OUTPUT     Write to OUTPUTFILE (default: stdout)\n");
  fprintf(stream, "    -r BYTES      Stop reading after READLIMIT bytes\n");
  fprintf(stream, "    -w BYTES      Stop writing after WRITELIMIT bytes\n");
  fprintf(stream, "    -v            Verbose mode (show status messages)\n");
  fprintf(stream, "    -h            Show this help message and exit\n");
#ifdef BUILD_TIMESTAMP
  fprintf(stream, "\n  Compiled on: %s. ©️ 2026, Ramón Barrios Láscar.\n",
          BUILD_TIMESTAMP);
#else
  // Fallback just in case you compile without the flag
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

  // Parse command line arguments
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

  // Validation
  if (listen_port && connect_hostport) {
    fprintf(stderr, PROG_NAME ": error: cannot use both -l and -c\n");
    fprintf(stderr, "©️ 2026, Ramón Barrios Láscar.\n");
    exit(EXIT_FAILURE);
  }
  if (!listen_port && !connect_hostport) {
    fprintf(stderr, PROG_NAME ": error: must specify either -l or -c\n");
    fprintf(stderr, "©️ 2026, Ramón Barrios Láscar.\n");
    exit(EXIT_FAILURE);
  }
  if (listen_port && infile) {
    fprintf(stderr, PROG_NAME
            ": error: cannot use -i with -l (server mode only receives)\n");
    fprintf(stderr, "©️ 2026, Ramón Barrios Láscar.\n");
    exit(EXIT_FAILURE);
  }

  // Gracefully handle termination signals
  struct sigaction sa;
  sa.sa_handler = graceful_shutdown;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;               // We want read/write to be interrupted (EINTR)
  sigaction(SIGHUP, &sa, NULL);  // kill -1
  sigaction(SIGINT, &sa, NULL);  // Ctrl+C
  sigaction(SIGTERM, &sa, NULL); // kill -15

  int fd_in = -1;
  int fd_out = -1;
  char *display_in = "stdin";
  char *display_out = "stdout";

  // Setup Server Mode (UDP Receive)
  if (listen_port) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM; // Changed to UDP
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

      if (bind(server_sock, p->ai_addr, p->ai_addrlen) == 0) {
        break; // Successfully bound
      }
      close(server_sock);
    }

    if (p == NULL) {
      fprintf(stderr, PROG_NAME ": error: failed to bind to UDP port %s\n",
              listen_port);
      exit(EXIT_FAILURE);
    }

    int is_ipv6 = (p->ai_family == AF_INET6);
    freeaddrinfo(res);

    // UDP does not use listen() or accept()
    // The server reads directly from the bound socket.
    fd_in = server_sock;

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
      if (write_limit > 0) {
        fprintf(stderr,
                PROG_NAME
                ": listening on UDP %s:%s and writing up to %llu bytes to %s\n",
                bind_addr, listen_port, write_limit, display_out);
      } else {
        fprintf(stderr,
                PROG_NAME ": listening on UDP %s:%s and writing to %s\n",
                bind_addr, listen_port, display_out);
      }

      // Block until the first UDP datagram arrives to grab the sender's IP
      struct sockaddr_storage client_addr;
      socklen_t client_len = sizeof(client_addr);
      char peek_buf[1];

      // MSG_PEEK lets us look at the packet without removing it from the OS
      // queue
      if (recvfrom(server_sock, peek_buf, 1, MSG_PEEK,
                   (struct sockaddr *)&client_addr, &client_len) >= 0) {
        char host_str[NI_MAXHOST], port_str[NI_MAXSERV];
        if (getnameinfo((struct sockaddr *)&client_addr, client_len, host_str,
                        sizeof(host_str), port_str, sizeof(port_str),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
          fprintf(stderr, PROG_NAME ": receiving datagrams from %s:%s\n",
                  host_str, port_str);
        }
      }
    }
  }

  // Setup Client Mode (UDP Send)
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
    hints.ai_socktype = SOCK_DGRAM; // Changed to UDP

    if (getaddrinfo(host, port, &hints, &res) != 0) {
      fprintf(stderr, PROG_NAME ": error: failed to resolve host %s\n", host);
      exit(EXIT_FAILURE);
    }

    for (p = res; p != NULL; p = p->ai_next) {
      fd_out = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd_out < 0)
        continue;

      // Connecting a UDP socket binds the default destination address
      // allowing us to use standard write() instead of sendto()
      if (connect(fd_out, p->ai_addr, p->ai_addrlen) == 0) {
        break;
      }
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
      fprintf(stderr, PROG_NAME ": UDP target set to %s:%s\n", host, port);
      if (read_limit > 0) {
        fprintf(stderr, PROG_NAME ": sending from %s %llu bytes to %s:%s\n",
                display_in, read_limit, host, port);
      } else {
        fprintf(stderr, PROG_NAME ": sending from %s to %s:%s\n", display_in,
                host, port);
      }
    }
    free(host);
  }

  // Main Copy Loop
  unsigned long long total_read = 0;
  unsigned long long total_written = 0;
  char buffer[BUFFER_SIZE];

  while (keep_running) {
    size_t bytes_to_read = BUFFER_SIZE;

    if (read_limit > 0 && (read_limit - total_read) < bytes_to_read) {
      bytes_to_read = read_limit - total_read;
    }
    if (read_limit > 0 && bytes_to_read == 0) {
      break;
    }
    if (write_limit > 0 && total_written >= write_limit) {
      break;
    }

    ssize_t n_read = read(fd_in, buffer, bytes_to_read);
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
      if (listen_port) {
        // In UDP, reading 0 bytes means an empty datagram was received.
        // It does not mean EOF. Keep listening.
        continue;
      } else {
        // If client, 0 bytes from local file/stdin means actual EOF.
        break;
      }
    }

    total_read += n_read;
    char *ptr = buffer;
    ssize_t remaining = n_read;

    if (write_limit > 0 && (total_written + remaining) > write_limit) {
      remaining = write_limit - total_written;
    }

    while (remaining > 0 && keep_running) {
      ssize_t n_written = write(fd_out, ptr, remaining);
      if (n_written < 0) {
        if (errno == EINTR) {
          if (!keep_running)
            goto loop_end;
          continue;
        }
        // UDP does not have EPIPE, but might throw ECONNREFUSED if ICMP
        // port unreachable is received on a connected UDP socket.
        perror(PROG_NAME ": write error");
        goto loop_end;
      }
      if (n_written == 0)
        goto loop_end;

      total_written += n_written;
      ptr += n_written;
      remaining -= n_written;
    }

    if (write_limit > 0 && total_written >= write_limit) {
      break;
    }
  }
loop_end:

  // Final output messages
  if (verbose) {
    if (listen_port) {
      fprintf(stderr,
              PROG_NAME
              ": received from UDP port %s and written %llu bytes to %s\n",
              listen_port, total_written, display_out);
    } else if (connect_hostport) {
      if (read_limit > 0 && total_written < read_limit) {
        fprintf(stderr, PROG_NAME ": %llu received of %llu sent to %s\n",
                total_written, read_limit, connect_hostport);
      } else if (read_limit > 0) {
        fprintf(stderr, PROG_NAME ": %llu bytes sent\n", total_written);
      } else {
        fprintf(stderr, PROG_NAME ": %llu bytes sent to %s\n", total_written,
                connect_hostport);
      }
    }
  }

  // Cleanup
  if (fd_in != STDIN_FILENO && fd_in >= 0)
    close(fd_in);
  if (fd_out != STDOUT_FILENO && fd_out >= 0)
    close(fd_out);

  return 0;
}
