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

#define BUFFER_SIZE 32768
#define PROG_NAME "netwcat"

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

void print_usage_and_exit(int status) {
  FILE *stream = (status == EXIT_SUCCESS) ? stdout : stderr;
  fprintf(stream, "Usage:\n");
  fprintf(stream,
          "  Server mode: " PROG_NAME " -l PORT [-o FILE] [-w WRITELIMIT] "
          "[-r READLIMIT] [-v]\n");
  fprintf(stream, "  Client mode: " PROG_NAME " -c HOST:PORT [-i FILE] [-w "
                  "WRITELIMIT] [-r READLIMIT] [-v]\n");
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

  // Ignore SIGPIPE so writing to a closed socket doesn't crash the program
  signal(SIGPIPE, SIG_IGN);

  int fd_in = -1;
  int fd_out = -1;
  char *display_in = "stdin";
  char *display_out = "stdout";

  // Setup Server Mode
  if (listen_port) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP socket
    hints.ai_flags = AI_PASSIVE; // Bind to the wildcard address (0.0.0.0 or ::)

    if (getaddrinfo(NULL, listen_port, &hints, &res) != 0) {
      fprintf(stderr, PROG_NAME ": error: failed to resolve listen address\n");
      exit(EXIT_FAILURE);
    }

    int server_sock = -1;
    // Loop through results and bind to the first one that works
    for (p = res; p != NULL; p = p->ai_next) {
      server_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (server_sock < 0)
        continue;

      int optval = 1;
      setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval,
                 sizeof(optval));

      // If IPv6, turn off IPV6_V6ONLY to ensure we also accept IPv4 clients
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
      fprintf(stderr, PROG_NAME ": error: failed to bind to port %s\n",
              listen_port);
      exit(EXIT_FAILURE);
    }

    // Determine which IP family we actually bound to for logging
    int is_ipv6 = (p->ai_family == AF_INET6);
    freeaddrinfo(res);

    if (listen(server_sock, 1) < 0) {
      perror(PROG_NAME ": listen");
      exit(EXIT_FAILURE);
    }

    // Output file resolution
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

    // Print listening banner
    if (verbose) {
      const char *bind_addr = is_ipv6 ? "::" : "0.0.0.0";
      if (write_limit > 0) {
        fprintf(stderr,
                PROG_NAME
                ": listening on %s:%s and writing up to %llu bytes to %s\n",
                bind_addr, listen_port, write_limit, display_out);
      } else {
        fprintf(stderr, PROG_NAME ": listening on %s:%s and writing to %s\n",
                bind_addr, listen_port, display_out);
      }
    }

    // Use sockaddr_storage to ensure we have enough memory to hold an IPv6
    // address
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    fd_in = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (fd_in < 0) {
      perror(PROG_NAME ": accept");
      exit(EXIT_FAILURE);
    }

    if (verbose) {
      char host_str[NI_MAXHOST];
      char port_str[NI_MAXSERV];
      // getnameinfo converts the raw binary address into readable strings for
      // both IPv4 and IPv6
      if (getnameinfo((struct sockaddr *)&client_addr, client_len, host_str,
                      sizeof(host_str), port_str, sizeof(port_str),
                      NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        fprintf(stderr, PROG_NAME ": connection from %s:%s received\n",
                host_str, port_str);
      } else {
        fprintf(stderr,
                PROG_NAME ": connection from unknown client received\n");
      }
    }

    close(server_sock); // We only handle one connection
  }

  // Setup Client Mode
  if (connect_hostport) {
    // Parse host and port
    char *host = strdup(connect_hostport);
    char *port = strchr(host, ':');
    if (!port) {
      fprintf(stderr, PROG_NAME ": error: invalid host:port format\n");
      exit(EXIT_FAILURE);
    }
    *port = '\0';
    port++;

    // Input file resolution
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
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
      fprintf(stderr, PROG_NAME ": error: failed to resolve host %s\n", host);
      exit(EXIT_FAILURE);
    }

    // Loop through results to connect
    for (p = res; p != NULL; p = p->ai_next) {
      fd_out = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd_out < 0)
        continue;

      if (connect(fd_out, p->ai_addr, p->ai_addrlen) == 0) {
        break; // Successfully connected
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

    // Print sending banner
    if (verbose) {
      fprintf(stderr, PROG_NAME ": connected to %s:%s\n", host, port);
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

  while (1) {
    size_t bytes_to_read = BUFFER_SIZE;

    // Apply read limit
    if (read_limit > 0 && (read_limit - total_read) < bytes_to_read) {
      bytes_to_read = read_limit - total_read;
    }
    if (read_limit > 0 && bytes_to_read == 0) {
      break; // Read limit reached
    }

    // Short-circuit if write limit is already reached
    if (write_limit > 0 && total_written >= write_limit) {
      break;
    }

    ssize_t n_read = read(fd_in, buffer, bytes_to_read);
    if (n_read < 0) {
      if (errno == EINTR)
        continue;
      perror(PROG_NAME ": read error");
      break;
    }
    if (n_read == 0) {
      break; // EOF
    }

    total_read += n_read;

    // Write what was read
    char *ptr = buffer;
    ssize_t remaining = n_read;

    // Truncate write block if write limit will be exceeded
    if (write_limit > 0 && (total_written + remaining) > write_limit) {
      remaining = write_limit - total_written;
    }

    while (remaining > 0) {
      ssize_t n_written = write(fd_out, ptr, remaining);
      if (n_written < 0) {
        if (errno == EINTR)
          continue;
        if (errno != EPIPE) {
          // EPIPE is expected if remote closes connection early
          perror(PROG_NAME ": write error");
        }
        goto loop_end;
      }
      if (n_written == 0)
        goto loop_end; // Should not happen in blocking mode

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
              ": received from port %s and written %llu bytes to %s\n",
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
