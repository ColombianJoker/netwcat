# netwcat

Simple `netcat` lookalike

## Using GCC (Linux, macOS, AIX Toolbox):

## Compiling

Simple

```sh
$ gcc -O2 -m64 -pthread netwcat.c -o netwcat
```

or even better

```sh
$ gcc -O2 -m64 -pthread -arch arm64 -arch x86_64 -o netwcat-mac-universal -DBUILD_TIMESTAMP="\"$(date +'%Y-%m-%d %H:%M:%S')\"" netwcat.c
$ gcc -O2 -m64 -pthread -arch arm64 -arch x86_64 -o neuwcat-mac-universal -DBUILD_TIMESTAMP="\"$(date +'%Y-%m-%d %H:%M:%S')\"" neuwcat.c
```

_Multi-threaded_ programs. These use a thread to send or receive from the network socket and another thread to read or write on the local file. Use the environment variable `CAT_BUFFER_SIZE` to size the internal memory buffer (`CAT_BUFFER_SIZE >= 10 485 760` recommended).

## Usage

### Neuwcat

```text
neuwcat-mac-universal
neuwcat: error: must specify either -l or -c
Usage:
  Server mode: neuwcat -l PORT [-o FILE] [-w WRITELIMIT] [-r READLIMIT] [-v]
  Client mode: neuwcat -c HOST:PORT [-i FILE] [-w WRITELIMIT] [-r READLIMIT] [-v]
  Options:
    -l PORT       Listen on local UDP port
    -c HOST:PORT  Send to remote HOST and UDP PORT
    -i INPUT      Read from INPUTFILE (default: stdin)
    -o OUTPUT     Write to OUTPUTFILE (default: stdout)
    -r BYTES      Stop reading after READLIMIT bytes
    -w BYTES      Stop writing after WRITELIMIT bytes
    -v            Verbose mode (show status messages)
    -h            Show this help message and exit

  Compiled on: 2026-07-22 15:05:24. ©️ 2026, Ramón Barrios Láscar.
```

### Netwcat

```text
/netwcat-mac-universal
netwcat: error: must specify either -l or -c
Usage:
  Server mode: netwcat -l PORT [-o FILE] [-w WRITELIMIT] [-r READLIMIT] [-v]
  Client mode: netwcat -c HOST:PORT [-i FILE] [-w WRITELIMIT] [-r READLIMIT] [-v]
  Options:
    -l PORT       Listen on local TCP port
    -c HOST:PORT  Connect to remote HOST and PORT
    -i INPUT      Read from INPUT (default: stdin)
    -o OUTPUT     Write to OUTPUT (default: stdout)
    -r BYTES      Stop reading after LIMIT bytes
    -w BYTES      Stop writing after LIMIT bytes
    -v            Verbose mode (show status messages)
    -h            Show this help message and exit

  Compiled on: 2026-07-22 15:38:30. ©️ 2026, Ramón Barrios Láscar.
```

## Examples

### 1. Basic File Transfer

Send exactly 1MB of zeroes from Host B to a file on Host A.

#### On Host A (serving/listening):

```sh
$ netwcat -l 1522 -o /tmp/test.1.bin -v
netwcat: listening on 0.0.0.0:1522 and writing to /tmp/test.1.bin
netwcat: connection from 192.168.1.50:43812 received
```

#### On Host B (client/sending):

```sh
$ netwcat -c host_A:1522 -r 1048576 -i /dev/zero -v
netwcat: connected to host_A:1522
netwcat: sending from /dev/zero 1048576 bytes to host_A:1522
netwcat: 1048576 bytes sent
```

### 2. Transfer with Limits via Standard I/O

Host A listens and stops writing after exactly 512KB. Host B attempts to send 1MB.

#### On Host A:

```sh
$ netwcat -l 1522 -w 524288 -v > /tmp/test2.bin
netwcat: listening on 0.0.0.0:1522 and writing up to 524288 bytes to stdout
netwcat: connection from 192.168.1.50:43814 received
```

#### On Host B:

```sh
$ netwcat -c host_A:1522 -r 1048576 -v < /dev/zero
netwcat: connected to host_A:1522
netwcat: sending from stdin 1048576 bytes to host_A:1522
netwcat: 524288 received of 1048576 sent to host_A:1522
```

_(Once the connection closes, Host A will output:)_

```sh
netwcat: received from port 1522 and written 524288 bytes to stdout
```

## License

MIT License.
