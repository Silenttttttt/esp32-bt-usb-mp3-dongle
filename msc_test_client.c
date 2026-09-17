/* Userspace BOT/SCSI test client for the Digispark fake-MSC firmware.
 *
 * The kernel's usb-storage driver refuses to bind to this device: Linux's
 * generic USB core silently rewrites our Low-Speed Bulk endpoints to
 * Interrupt type (a mandatory spec-compliance fixup that happens before any
 * driver's probe() runs), and usb-storage then requires genuine Bulk
 * endpoints to attach. That's a Linux usb-storage/usbcore contradiction,
 * not a firmware bug -- so this talks to the device directly over libusb,
 * using interrupt transfers (matching what the kernel already reports the
 * endpoint type as), to prove the firmware's BOT+SCSI+FAT logic actually
 * works end-to-end, independent of whether the OS will ever auto-mount it.
 */
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VID 0x16d0
#define PID 0x05dc
#define EP_OUT 0x01
#define EP_IN  0x81
#define TIMEOUT_MS 3000

static libusb_device_handle *dev;
static uint32_t tag_counter = 1;

static void hexdump_ascii(const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char c = buf[i];
        putchar((c >= 32 && c < 127) ? c : '.');
    }
    putchar('\n');
}

static int send_cbw(uint8_t cbLen, const uint8_t *cdb, uint32_t dataLen, int dirIn)
{
    uint8_t cbw[31] = {0};
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    uint32_t tag = tag_counter++;
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &dataLen, 4);
    cbw[12] = dirIn ? 0x80 : 0x00;
    cbw[13] = 0; /* LUN */
    cbw[14] = cbLen;
    memcpy(cbw + 15, cdb, cbLen);

    int transferred;
    int rc = 0;
    for (int off = 0; off < 31; ) {
        int chunk = (31 - off) < 8 ? (31 - off) : 8;
        rc = libusb_interrupt_transfer(dev, EP_OUT, cbw + off, chunk, &transferred, TIMEOUT_MS);
        if (rc != 0) { fprintf(stderr, "CBW send error: %s\n", libusb_error_name(rc)); return rc; }
        off += chunk;
    }
    return 0;
}

static int recv_bytes(uint8_t *buf, int total)
{
    int transferred;
    for (int off = 0; off < total; ) {
        int chunk = (total - off) < 8 ? (total - off) : 8;
        int rc = libusb_interrupt_transfer(dev, EP_IN, buf + off, chunk, &transferred, TIMEOUT_MS);
        if (rc != 0) { fprintf(stderr, "IN transfer error at off %d: %s\n", off, libusb_error_name(rc)); return rc; }
        off += transferred;
        if (transferred < chunk) break; /* short packet = end of this phase */
    }
    return 0;
}

static int do_command(const char *label, uint8_t cbLen, const uint8_t *cdb, uint32_t dataLen, int dirIn, uint8_t *outBuf)
{
    printf("--- %s ---\n", label);
    if (send_cbw(cbLen, cdb, dataLen, dirIn) != 0) return -1;

    uint8_t dataBuf[600];
    if (dataLen > 0 && dirIn) {
        if (recv_bytes(dataBuf, dataLen) != 0) return -1;
        if (outBuf) memcpy(outBuf, dataBuf, dataLen);
        printf("data (%u bytes): ", dataLen);
        hexdump_ascii(dataBuf, dataLen);
    }

    uint8_t csw[13];
    if (recv_bytes(csw, 13) != 0) return -1;
    printf("CSW status=%d residue=%u\n", csw[12],
           (unsigned)(csw[8] | (csw[9] << 8) | (csw[10] << 16) | (csw[11] << 24)));
    return csw[12];
}

int main(void)
{
    libusb_init(NULL);
    dev = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dev) { fprintf(stderr, "device not found (%04x:%04x)\n", VID, PID); return 1; }

    libusb_set_auto_detach_kernel_driver(dev, 1);
    int rc = libusb_claim_interface(dev, 0);
    if (rc != 0) { fprintf(stderr, "claim_interface failed: %s\n", libusb_error_name(rc)); return 1; }

    uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    do_command("TEST UNIT READY", 6, tur, 0, 1, NULL);

    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t inqData[36];
    do_command("INQUIRY", 6, inq, 36, 1, inqData);

    uint8_t rc10[10] = {0x25, 0,0,0,0,0,0,0,0,0};
    uint8_t capData[8];
    do_command("READ CAPACITY 10", 10, rc10, 8, 1, capData);
    uint32_t lastLba = (capData[0] << 24) | (capData[1] << 16) | (capData[2] << 8) | capData[3];
    uint32_t blockSize = (capData[4] << 24) | (capData[5] << 16) | (capData[6] << 8) | capData[7];
    printf("=> last LBA = %u, block size = %u\n\n", lastLba, blockSize);

    /* read all sectors (0..lastLba) and print them as text */
    uint8_t read10[10] = {0x28, 0, 0,0,0,0, 0, 0,(uint8_t)(lastLba+1), 0};
    /* LBA = 0 (bytes 2-5), transfer length = lastLba+1 sectors (bytes 7-8, big-endian) */
    read10[7] = ((lastLba + 1) >> 8) & 0xFF;
    read10[8] = (lastLba + 1) & 0xFF;
    uint32_t totalBytes = (lastLba + 1) * blockSize;
    uint8_t *diskBuf = malloc(totalBytes);
    printf("--- READ10 (LBA 0, %u sectors, %u bytes) ---\n", lastLba + 1, totalBytes);
    if (send_cbw(10, read10, totalBytes, 1) == 0 && recv_bytes(diskBuf, totalBytes) == 0) {
        uint8_t csw[13];
        recv_bytes(csw, 13);
        printf("CSW status=%d\n\n", csw[12]);
        printf("=== Full disk image content (raw, printable chars only) ===\n");
        for (uint32_t i = 0; i < totalBytes; i++) {
            unsigned char c = diskBuf[i];
            if (c >= 32 && c < 127) putchar(c);
            else if (c == '\n' || c == '\r') putchar(c);
        }
        printf("\n=== end ===\n");
    }
    free(diskBuf);

    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(NULL);
    return 0;
}
