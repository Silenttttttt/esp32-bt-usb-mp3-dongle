/* Stage 1 test firmware: Digispark (ATtiny85 + V-USB) pretending to be a
 * USB Mass Storage (Bulk-Only Transport / SCSI) flash drive containing one
 * tiny static file. This declares Bulk endpoints on a Low-Speed V-USB
 * device, which the USB spec forbids -- it may or may not enumerate,
 * that's exactly what this test is for. */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <string.h>

#include "usbdrv.h"
#include "disk_image.h"

/* --------------------- Stage 3: serial RX from ESP32 -------------------
 * Plain polled bit-bang UART receive on P2 (PB2), ESP32 TX -> here.
 * One direction only. Sampled only while idle between BOT commands (see
 * main loop) so a ~9ms blocking receive never collides with an in-flight
 * USB transfer -- V-USB tolerates gaps up to ~50ms between usbPoll() calls,
 * and this only blocks when a start bit is actually seen, which happens at
 * whatever rate the ESP32 sends (about once/sec in the test sketch). */
#define RX_DDR   DDRB
#define RX_PORT  PORTB
#define RX_PIN_REG PINB
#define RX_BIT   PB2
#define RX_BAUD  1200UL
#define RX_BIT_US (1000000UL / RX_BAUD)

static uint8_t rxBuf[48];
static uint8_t rxLen;

/* live-received bytes are overlaid at the start of the file's one data
 * cluster (LBA 3, per gen_fat_image.py's "data starts at LBA 3" output) */
#define RX_OVERLAY_OFFSET (3UL * DISK_SECTOR_SIZE)

static void check_and_receive_serial(void)
{
    if (RX_PIN_REG & (1 << RX_BIT)) return; /* idle high, no start bit */
    _delay_us(RX_BIT_US + RX_BIT_US / 2);   /* skip start bit, land mid data-bit-0 */
    uint8_t byte = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (RX_PIN_REG & (1 << RX_BIT)) byte |= (1 << i);
        _delay_us(RX_BIT_US);
    }
    /* Don't allow immediate re-triggering on the tail of this same frame
     * (stop bit / inter-byte gap) if we started mid-transmission -- wait
     * (bounded) for the line to return to true idle-high first. Without
     * this, one mis-synced start can cascade into several garbage bytes. */
    for (uint16_t guard = 0; guard < 2000 && !(RX_PIN_REG & (1 << RX_BIT)); guard++) {
        _delay_us(1);
    }
    if (rxLen < sizeof(rxBuf)) {
        rxBuf[rxLen++] = byte;
    } else {
        memmove(rxBuf, rxBuf + 1, sizeof(rxBuf) - 1);
        rxBuf[sizeof(rxBuf) - 1] = byte;
    }
}

#define BOT_GET_MAX_LUN 0xFE
#define BOT_RESET       0xFF

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_MODE_SENSE6     0x1A
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

typedef enum {
    BOT_STATE_CBW = 0,
    BOT_STATE_DATA_IN,
    BOT_STATE_DATA_OUT,
    BOT_STATE_STATUS,
} bot_state_t;

static bot_state_t botState = BOT_STATE_CBW;

static uint8_t cbw[31];
static uint8_t cbwFill;

static uint32_t cbwTag;
static uint8_t  cbwCb[16];

static uint8_t ramBuf[36];
static uint8_t ramLen;
static uint8_t ramPos;

static uint8_t  streamingSector;
static uint32_t sectorLba;
static uint16_t sectorsLeft;
static uint16_t sectorByteOff;

static uint32_t writeDiscardLeft;

/* ------------------------- USB descriptors ------------------------- */

static const PROGMEM uint8_t configDescrMSC[32] = {
    9, USBDESCR_CONFIG, 32, 0, 1, 1, 0,
    (1 << 7), USB_CFG_MAX_BUS_POWER / 2,

    9, USBDESCR_INTERFACE, 0, 0, 2,
    USB_CFG_INTERFACE_CLASS, USB_CFG_INTERFACE_SUBCLASS, USB_CFG_INTERFACE_PROTOCOL, 0,

    7, USBDESCR_ENDPOINT, 0x01, 0x02, 8, 0, 0, /* OUT ep1, bulk, 8 bytes */
    7, USBDESCR_ENDPOINT, 0x81, 0x02, 8, 0, 0, /* IN  ep1, bulk, 8 bytes */
};

uchar usbFunctionDescriptor(usbRequest_t *rq)
{
    if (rq->wValue.bytes[1] == USBDESCR_DEVICE) {
        usbMsgPtr = (uchar *)usbDescriptorDevice;
        return usbDescriptorDevice[0];
    }
    usbMsgPtr = (uchar *)configDescrMSC;
    return sizeof(configDescrMSC);
}

usbMsgLen_t usbFunctionSetup(uchar data[8])
{
    usbRequest_t *rq = (usbRequest_t *)(void *)data;

    if ((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS) {
        if (rq->bRequest == BOT_GET_MAX_LUN) {
            static uint8_t maxLun = 0;
            usbMsgPtr = &maxLun;
            return 1;
        }
        if (rq->bRequest == BOT_RESET) {
            botState = BOT_STATE_CBW;
            cbwFill = 0;
            return 0;
        }
    }
    return 0;
}

/* ------------------------------ BOT/SCSI ----------------------------- */

static void csw_prepare(uint8_t status)
{
    ramBuf[0] = 0x55; ramBuf[1] = 0x53; ramBuf[2] = 0x42; ramBuf[3] = 0x53; /* "USBS" */
    memcpy(ramBuf + 4, &cbwTag, 4);
    ramBuf[8] = ramBuf[9] = ramBuf[10] = ramBuf[11] = 0; /* residue = 0 */
    ramBuf[12] = status;
    ramLen = 13;
    ramPos = 0;
    streamingSector = 0;
    botState = BOT_STATE_STATUS;
}

static void begin_ram_data_in(uint8_t len)
{
    ramLen = len;
    ramPos = 0;
    streamingSector = 0;
    botState = BOT_STATE_DATA_IN;
}

static void begin_sector_data_in(uint32_t lba, uint16_t count)
{
    sectorLba = lba;
    sectorsLeft = count;
    sectorByteOff = 0;
    streamingSector = 1;
    botState = BOT_STATE_DATA_IN;
}

static void handle_cbw(void)
{
    if (cbw[0] != 0x55 || cbw[1] != 0x53 || cbw[2] != 0x42 || cbw[3] != 0x43) {
        return; /* not "USBC", drop and wait for a fresh CBW */
    }
    memcpy(&cbwTag, cbw + 4, 4);
    uint32_t dataLen;
    memcpy(&dataLen, cbw + 8, 4);
    memcpy(cbwCb, cbw + 15, 16);

    switch (cbwCb[0]) {
    case SCSI_TEST_UNIT_READY:
        csw_prepare(0);
        break;

    case SCSI_REQUEST_SENSE:
        memset(ramBuf, 0, 18);
        ramBuf[0] = 0x70;
        ramBuf[7] = 10;
        begin_ram_data_in(18);
        break;

    case SCSI_INQUIRY:
        memset(ramBuf, 0, 36);
        ramBuf[1] = 0x80; /* removable */
        ramBuf[2] = 0x00;
        ramBuf[3] = 0x01;
        ramBuf[4] = 31;
        memcpy(ramBuf + 8, "DIGISPRK", 8);
        memcpy(ramBuf + 16, "FAKE MSC TEST   ", 16);
        memcpy(ramBuf + 32, "1.0 ", 4);
        begin_ram_data_in(36);
        break;

    case SCSI_READ_CAPACITY10: {
        uint32_t lastLba = DISK_TOTAL_SECTORS - 1;
        ramBuf[0] = (lastLba >> 24) & 0xFF;
        ramBuf[1] = (lastLba >> 16) & 0xFF;
        ramBuf[2] = (lastLba >> 8) & 0xFF;
        ramBuf[3] = lastLba & 0xFF;
        ramBuf[4] = 0; ramBuf[5] = 0;
        ramBuf[6] = (DISK_SECTOR_SIZE >> 8) & 0xFF;
        ramBuf[7] = DISK_SECTOR_SIZE & 0xFF;
        begin_ram_data_in(8);
        break;
    }

    case SCSI_MODE_SENSE6:
        ramBuf[0] = 3; ramBuf[1] = 0; ramBuf[2] = 0x80; ramBuf[3] = 0;
        begin_ram_data_in(4);
        break;

    case SCSI_READ10: {
        uint32_t lba = ((uint32_t)cbwCb[2] << 24) | ((uint32_t)cbwCb[3] << 16) |
                       ((uint32_t)cbwCb[4] << 8) | cbwCb[5];
        uint16_t count = ((uint16_t)cbwCb[7] << 8) | cbwCb[8];
        if (lba >= DISK_TOTAL_SECTORS) {
            csw_prepare(1);
            break;
        }
        if ((uint32_t)lba + count > DISK_TOTAL_SECTORS) {
            count = DISK_TOTAL_SECTORS - lba;
        }
        begin_sector_data_in(lba, count);
        break;
    }

    case SCSI_WRITE10:
        writeDiscardLeft = dataLen;
        if (writeDiscardLeft == 0) {
            csw_prepare(0);
        } else {
            botState = BOT_STATE_DATA_OUT;
        }
        break;

    default:
        csw_prepare(1);
        break;
    }
}

void usbFunctionWriteOut(uchar *data, uchar len)
{
    if (botState == BOT_STATE_CBW) {
        uint8_t room = sizeof(cbw) - cbwFill;
        uint8_t n = (len < room) ? len : room;
        memcpy(cbw + cbwFill, data, n);
        cbwFill += n;
        if (cbwFill >= sizeof(cbw)) {
            handle_cbw();
            cbwFill = 0;
        }
    } else if (botState == BOT_STATE_DATA_OUT) {
        uint32_t n = len;
        if (n > writeDiscardLeft) n = writeDiscardLeft;
        writeDiscardLeft -= n;
        if (writeDiscardLeft == 0) {
            csw_prepare(0);
        }
    }
}

/* --------------------------------- main -------------------------------- */

int main(void)
{
    wdt_disable();

    RX_DDR &= ~(1 << RX_BIT); /* P2 as input, ESP32 drives it */

    usbInit();
    usbDeviceDisconnect();
    _delay_ms(250);
    usbDeviceConnect();

    sei();

    for (;;) {
        usbPoll();

        if (botState == BOT_STATE_CBW) {
            check_and_receive_serial();
        }

        if (!usbInterruptIsReady()) continue;

        if (botState == BOT_STATE_DATA_IN && streamingSector) {
            if (sectorsLeft == 0) continue;
            uint8_t chunk[8];
            uint8_t n = 0;
            while (n < 8) {
                uint32_t byteAddr = sectorLba * DISK_SECTOR_SIZE + sectorByteOff;
                if (byteAddr >= RX_OVERLAY_OFFSET && byteAddr < RX_OVERLAY_OFFSET + rxLen) {
                    chunk[n] = rxBuf[byteAddr - RX_OVERLAY_OFFSET];
                } else {
                    chunk[n] = (byteAddr < sizeof(disk_image)) ? pgm_read_byte(&disk_image[byteAddr]) : 0;
                }
                n++;
                sectorByteOff++;
                if (sectorByteOff >= DISK_SECTOR_SIZE) {
                    sectorByteOff = 0;
                    sectorLba++;
                    sectorsLeft--;
                    if (sectorsLeft == 0) break;
                }
            }
            usbSetInterrupt(chunk, n);
            if (sectorsLeft == 0 && sectorByteOff == 0) {
                csw_prepare(0);
            }
        } else if (botState == BOT_STATE_DATA_IN || botState == BOT_STATE_STATUS) {
            uint8_t remaining = ramLen - ramPos;
            uint8_t n = (remaining < 8) ? remaining : 8;
            usbSetInterrupt(ramBuf + ramPos, n);
            ramPos += n;
            if (ramPos >= ramLen) {
                if (botState == BOT_STATE_DATA_IN) {
                    csw_prepare(0);
                } else {
                    botState = BOT_STATE_CBW;
                    cbwFill = 0;
                }
            }
        }
    }
}
