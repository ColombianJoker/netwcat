# netwcat

Simple `netcat` lookalike

## Using GCC (Linux, macOS, AIX Toolbox):

```sh
# gcc -O2 -m64 netwcat.c -o netwcat
```

# Usage

```text
Usage:
Server mode: netwcat -l PORT [-o FILE] [-w WRITELIMIT] [-r READLIMIT]
Client mode: netwcat -c HOST:PORT [-i FILE] [-w WRITELIMIT] [-r READLIMIT]
Options:
-l PORT Listen on local TCP port
-c HOST:PORT Connect to remote HOST and PORT
-i FILE Read from INPUTFILE (default: stdin)
-o FILE Write to OUTPUTFILE (default: stdout)
-r BYTES Stop reading after READLIMIT bytes
-w BYTES Stop writing after WRITELIMIT bytes
-h Show this help message and exit
```

# Examples

## 1. Basic File Transfer

Send exactly 1MB of zeroes from Host B to a file on Host A.

### On Host A (serving/listening):

```sh
$ netwcat -l 1522 -o /tmp/test.1.bin
netwcat: listening on 0.0.0.0:1522 and writing to /tmp/test.1.bin
```

### On Host B (client/sending):

```sh
$ netwcat -c host_A:1522 -r 1048576 -i /dev/zero
netwcat: sending from /dev/zero 1048576 bytes to host_A:1522
netwcat: 1048576 bytes sent
```

## 2. Transfer with Limits via Standard I/O

Host A listens and stops writing after exactly 512KB. Host B attempts to send 1MB.

### On Host A:

```sh
$ netwcat -l 1522 -w 524288 > /tmp/test2.bin
netwcat: listening on 0.0.0.0:1522 and writing up to 524288 bytes to stdout
```

### On Host B:

```sh
$ netwcat -c host_A:1522 -r 1048576 < /dev/zero
netwcat: sending from stdin 1048576 bytes to host_A:1522
netwcat: 524288 received of 1048576 sent to host_A:1522
```

_(Once the connection closes, Host A will output:)_

```sh
netwcat: received from port 1522 and written 524288 bytes to stdout
```

# License

MIT License.
