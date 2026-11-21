/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Linux HCI user-channel ABI. The musl sysroot has no BlueZ headers. */
#define BTPROTO_HCI 1
#define HCI_CHANNEL_USER 1

struct hci_address {
	sa_family_t family;
	uint16_t index;
	uint16_t channel;
};

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int receive_event(int fd, uint8_t *event, size_t size, int64_t deadline)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	for (;;) {
		int64_t remaining = deadline - now_ms();
		int ready = remaining > 0 ? poll(&pfd, 1, (int)remaining) : 0;
		ssize_t n;

		if (!ready) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		n = read(fd, event, size);
		if (n <= 0) {
			if (!n)
				errno = EIO;
			return -1;
		}
		if (n >= 3 && event[0] == 0x04 && n == event[2] + 3)
			return (int)n;
	}
}

static int hci_command(int fd, uint16_t opcode, const uint8_t *parameters,
		       uint8_t length, uint8_t *reply, size_t reply_size)
{
	uint8_t command[4 + UINT8_MAX] = { 0x01, opcode & 0xff, opcode >> 8, length };
	uint8_t event[3 + UINT8_MAX];
	int64_t deadline = now_ms() + 4000;
	ssize_t written;
	int n;

	if (length)
		memcpy(command + 4, parameters, length);
	written = write(fd, command, length + 4);
	if (written < 0)
		goto fail;
	if (written != length + 4) {
		errno = EIO;
		goto fail;
	}
	while ((n = receive_event(fd, event, sizeof(event), deadline)) >= 0) {
		if (n < 7 || event[1] != 0x0e ||
		    event[4] != command[1] || event[5] != command[2])
			continue;
		if (event[6]) {
			fprintf(stderr, "HCI opcode 0x%04x: status 0x%02x\n", opcode, event[6]);
			return -1;
		}
		if ((size_t)(n - 7) < reply_size) {
			errno = EPROTO;
			goto fail;
		}
		if (reply_size)
			memcpy(reply, event + 7, reply_size);
		return 0;
	}
fail:
	fprintf(stderr, "HCI opcode 0x%04x: %s\n", opcode, strerror(errno));
	return -1;
}

static unsigned le16(const uint8_t *bytes)
{
	return bytes[0] | (unsigned)bytes[1] << 8;
}

static void print_address(const uint8_t *a)
{
	printf("%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static int show_info(int fd)
{
	uint8_t version[8], address[6];

	if (hci_command(fd, 0x1001, NULL, 0, version, sizeof(version)) ||
	    hci_command(fd, 0x1009, NULL, 0, address, sizeof(address)))
		return -1;
	printf("hci0: HCI 0x%02x rev 0x%04x, LMP 0x%02x subver 0x%04x, manufacturer 0x%04x\n",
	       version[0], le16(version + 1), version[3], le16(version + 6), le16(version + 4));
	printf("hci0: controller BD_ADDR ");
	print_address(address);
	putchar('\n');
	return 0;
}

static unsigned show_advertisements(const uint8_t *event, int size)
{
	const uint8_t *report = event + 5;
	const uint8_t *end = event + size;
	unsigned count = 0;

	if (size < 5 || event[1] != 0x3e || event[3] != 0x02)
		return 0;
	for (unsigned i = 0; i < event[4] && end - report >= 10; i++) {
		size_t length = report[8];

		if ((size_t)(end - report) < length + 10)
			break;
		printf("LE advertisement ");
		print_address(report + 2);
		printf(" type=%u address_type=%u rssi=%d data=", report[0], report[1],
		       (int)(int8_t)report[9 + length]);
		for (size_t j = 0; j < length; j++)
			printf("%02x", report[9 + j]);
		putchar('\n');
		report += length + 10;
		count++;
	}
	return count;
}

static int passive_scan(int fd, unsigned seconds)
{
	/* Passive scan, 60 ms interval and window, all advertising addresses. */
	const uint8_t parameters[] = { 0, 0x60, 0, 0x60, 0, 0, 0 };
	const uint8_t mask[] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	const uint8_t le_mask[] = { 0x02, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t start[] = { 1, 1 }, stop[] = { 0, 0 };
	uint8_t event[3 + UINT8_MAX];
	int64_t deadline;
	unsigned count = 0;
	int size, scan_errno;

	if (hci_command(fd, 0x0c01, mask, sizeof(mask), NULL, 0) ||
	    hci_command(fd, 0x2001, le_mask, sizeof(le_mask), NULL, 0) ||
	    hci_command(fd, 0x200b, parameters, sizeof(parameters), NULL, 0) ||
	    hci_command(fd, 0x200c, start, sizeof(start), NULL, 0))
		return -1;
	deadline = now_ms() + seconds * 1000;
	printf("BLE passive scan for %u s\n", seconds);
	while ((size = receive_event(fd, event, sizeof(event), deadline)) >= 0)
		count += show_advertisements(event, size);
	scan_errno = errno;
	if (hci_command(fd, 0x200c, stop, sizeof(stop), NULL, 0))
		return -1;
	if (scan_errno != ETIMEDOUT) {
		fprintf(stderr, "BLE scan: %s\n", strerror(scan_errno));
		return -1;
	}
	printf("BLE passive scan: %u advertising reports\n", count);
	return count ? 0 : -1;
}

int main(int argc, char **argv)
{
	const struct hci_address address = { AF_BLUETOOTH, 0, HCI_CHANNEL_USER };
	unsigned long seconds = 0;
	int fd, result;

	if (argc > 2) {
		fprintf(stderr, "Usage: %s [passive-scan-seconds (1-60)]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 2) {
		char *end;

		seconds = strtoul(argv[1], &end, 10);
		if (!seconds || seconds > 60 || *end) {
			fprintf(stderr, "Usage: %s [passive-scan-seconds (1-60)]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}
	fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (fd < 0) {
		perror("Bluetooth socket");
		return EXIT_FAILURE;
	}
	if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("hci0 user channel");
		close(fd);
		return EXIT_FAILURE;
	}
	result = show_info(fd);
	if (!result && seconds)
		result = passive_scan(fd, seconds);
	close(fd);
	return result ? EXIT_FAILURE : EXIT_SUCCESS;
}
