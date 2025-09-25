// rtt_test.c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>

#define TEST_BYTE       0xA5
#define TIMEOUT_SEC     1

static double time_diff_us(struct timeval start, struct timeval end) {
    return (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_usec - start.tv_usec);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <serial-device>\n", argv[0]);
        return 1;
    }

    const char *port = argv[1];
    int fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        return 1;
    }

    cfsetospeed(&tty, B19200);
    cfsetispeed(&tty, B19200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
    tty.c_iflag &= ~IGNBRK;                     // disable break processing
    tty.c_lflag = 0;                            // no signaling chars, no echo
    tty.c_oflag = 0;                            // no remapping, no delays
    tty.c_cc[VMIN]  = 0;                        // non-blocking read
    tty.c_cc[VTIME] = 0;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);     // shut off xon/xoff ctrl
    tty.c_cflag |= (CLOCAL | CREAD);            // ignore modem controls
    tty.c_cflag &= ~(PARENB | PARODD);          // no parity
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(fd);
        return 1;
    }

    // flush any old data
    tcflush(fd, TCIOFLUSH);

    // send test byte
    unsigned char tx = TEST_BYTE;
    ssize_t wlen = write(fd, &tx, 1);
    if (wlen != 1) {
        perror("write");
        close(fd);
        return 1;
    }

    struct timeval start, end;
    gettimeofday(&start, NULL);

    fd_set rfds;
    struct timeval timeout;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = 0;

    int ret = select(fd + 1, &rfds, NULL, NULL, &timeout);
    if (ret == -1) {
        perror("select");
        close(fd);
        return 1;
    } else if (ret == 0) {
        fprintf(stderr, "Timeout waiting for response.\n");
        close(fd);
        return 1;
    }

    unsigned char rx;
    ssize_t rlen = read(fd, &rx, 1);
    gettimeofday(&end, NULL);

    if (rlen == 1 && rx == TEST_BYTE) {
        printf("RTT: %.2f microseconds\n", time_diff_us(start, end));
    } else {
        fprintf(stderr, "Received invalid or no byte.\n");
    }

    close(fd);
    return 0;
}
