"""The one boundary interface between board and radio: SCSI READ10-style
sector reads over a plain socket. This is deliberately the layer right
above raw USB BOT/SCSI framing (already proven separately, on real
hardware, via msc_test_client.c) -- it's the same abstraction a real
kernel's usb-storage driver hands up to a filesystem driver.
"""
import struct

SECTOR_SIZE = 512
_REQ_FMT = ">BIH"  # opcode(1) + lba(4, big-endian) + count(2, big-endian, unused for WATERMARK)
_REQ_LEN = struct.calcsize(_REQ_FMT)
OPCODE_READ10 = 0x01
OPCODE_WATERMARK = 0x02  # how many bytes of the data region are genuinely valid so far


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        more = sock.recv(n - len(data))
        if not more:
            raise ConnectionError("connection closed mid-read")
        data += more
    return data


def send_read10(sock, lba, count):
    sock.sendall(struct.pack(_REQ_FMT, OPCODE_READ10, lba, count))


def recv_read10_request(sock):
    header = recv_exact(sock, _REQ_LEN)
    opcode, lba, count = struct.unpack(_REQ_FMT, header)
    return opcode, lba, count


def read_sectors(sock, lba, count):
    send_read10(sock, lba, count)
    return recv_exact(sock, count * SECTOR_SIZE)


def query_watermark(sock):
    """Ask how many bytes of the growing file's data region are genuinely
    encoded so far. Real hardware wouldn't need this exact call (it just
    wouldn't return not-yet-encoded bytes for a well-designed SCSI target),
    but reading zero-content isn't safe to use as an unencoded-data signal
    -- legitimate encoded audio can itself contain zero bytes -- so the
    reader needs an explicit, unambiguous answer instead of guessing from
    content."""
    sock.sendall(struct.pack(_REQ_FMT, OPCODE_WATERMARK, 0, 0))
    return int.from_bytes(recv_exact(sock, 8), "big")
