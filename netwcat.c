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

void print_usage_and_exit(int status) {
  FILE *stream = (status == EXIT_SUCCESS) ? stdout : stderr;
  fprintf(stream, "Usage:\n");
  fprintf(stream, "  Server mode: netwcat -l PORT [-o FILE] [-w WRITELIMIT] "
                  "[-r READLIMIT] [-v]\n");
  fprintf(stream, "  Client mode: netwcat -c HOST:PORT [-i FILE] [-w "
                  "WRITELIMIT] [-r READLIMIT] [-v]\n");
  fprintf(stream, "  Options:\n");
  fprintf(stream, "    -l PORT       Listen on local TCP port\n");
  fprintf(stream, "    -c HOST:PORT  Connect to remote HOST and PORT\n");
  fprintf(stream, "    -i FILE       Read from INPUTFILE (default: stdin)\n");
  fprintf(stream, "    -o FILE       Write to OUTPUTFILE (default: stdout)\n");
  fprintf(stream, "    -r BYTES      Stop reading after READLIMIT bytes\n");
  fprintf(stream, "    -w BYTES      Stop writing after WRITELIMIT bytes\n");
  fprintf(stream, "    -v            Verbose mode (show status messages)\n");
  fprintf(stream, "    -h            Show this help message and exit\n");
  fprintf(stream, "\n©️ 2026, Ramón Barrios Láscar.\n");
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
    fprintf(stderr, "netwcat: error: cannot use both -l and -c\n");
    exit(EXIT_FAILURE);
  }
  if (!listen_port && !connect_hostport) {
    fprintf(stderr, "netwcat: error: must specify either -l or -c\n");
    exit(EXIT_FAILURE);
  }
  if (listen_port && infile) {
    fprintf(
        stderr,
        "netwcat: error: cannot use -i with -l (server mode only receives)\n");
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
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
      perror("socket");
      exit(EXIT_FAILURE);
    }

    int optval = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(atoi(listen_port));

    if (bind(server_sock, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
      perror("bind");
      exit(EXIT_FAILURE);
    }
    if (listen(server_sock, 1) < 0) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    // Output file resolution
    if (outfile && strcmp(outfile, "-") != 0) {
      fd_out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd_out < 0) {
        perror("open output file");
        exit(EXIT_FAILURE);
      }
      display_out = outfile;
    } else {
      fd_out = STDOUT_FILENO;
    }

    // Print listening banner
    if (verbose) {
      if (write_limit > 0) {
        fprintf(stderr,
                "netwcat: listening on 0.0.0.0:%s and writing up to %llu bytes "
                "to %s\n",
                listen_port, write_limit, display_out);
      } else {
        fprintf(stderr, "netwcat: listening on 0.0.0.0:%s and writing to %s\n",
                listen_port, display_out);
      }
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    fd_in = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (fd_in < 0) {
      perror("accept");
      exit(EXIT_FAILURE);
    }

    if (verbose) {
      fprintf(stderr, "netwcat: connection from %s:%d received\n",
              inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }

    close(server_sock); // We only handle one connection
  }

  // Setup Client Mode
  if (connect_hostport) {
    // Parse host and port
    char *host = strdup(connect_hostport);
    char *port = strchr(host, ':');
    if (!port) {
      fprintf(stderr, "netwcat: error: invalid host:port format\n");
      exit(EXIT_FAILURE);
    }
    *port = '\0';
    port++;

    // Input file resolution
    if (infile && strcmp(infile, "-") != 0) {
      fd_in = open(infile, O_RDONLY);
      if (fd_in < 0) {
        perror("open input file");
        exit(EXIT_FAILURE);
      }
      display_in = infile;
    } else {
      fd_in = STDIN_FILENO;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
      fprintf(stderr, "netwcat: error: failed to resolve host %s\n", host);
      exit(EXIT_FAILURE);
    }

    fd_out = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_out < 0) {
      perror("socket");
      exit(EXIT_FAILURE);
    }

    if (connect(fd_out, res->ai_addr, res->ai_addrlen) < 0) {
      perror("connect");
      exit(EXIT_FAILURE);
    }
    freeaddrinfo(res);

    // Print sending banner
    if (verbose) {
      fprintf(stderr, "netwcat: connected to %s\n", connect_hostport);
      if (read_limit > 0) {
        fprintf(stderr, "netwcat: sending from %s %llu bytes to %s\n",
                display_in, read_limit, connect_hostport);
      } else {
        fprintf(stderr, "netwcat: sending from %s to %s\n", display_in,
                connect_hostport);
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
      perror("read error");
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
          perror("write error");
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
              "netwcat: received from port %s and written %llu bytes to %s\n",
              listen_port, total_written, display_out);
    } else if (connect_hostport) {
      if (read_limit > 0 && total_written < read_limit) {
        fprintf(stderr, "netwcat: %llu received of %llu sent to %s\n",
                total_written, read_limit, connect_hostport);
      } else if (read_limit > 0) {
        fprintf(stderr, "netwcat: %llu bytes sent\n", total_written);
      } else {
        fprintf(stderr, "netwcat: %llu bytes sent to %s\n", total_written,
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
