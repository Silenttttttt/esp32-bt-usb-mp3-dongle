import struct

FRAME_MAGIC = 0xAA
MAX_FRAME_LEN = 4096


class MockSerial:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def read(self, n):
        chunk = self.data[self.pos:self.pos + n]
        self.pos += len(chunk)
        return chunk


def read_exact(ser, n):
    data = b""
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def find_sync(ser):
    while True:
        b = read_exact(ser, 1)
        if b[0] == FRAME_MAGIC:
            return


def parse_stream(ser):
    frames = []
    try:
        while True:
            find_sync(ser)
            header = read_exact(ser, 5)
            frame_type = chr(header[0])
            length = struct.unpack(">I", header[1:5])[0]
            if frame_type not in ("A", "C") or length > MAX_FRAME_LEN:
                continue
            payload = read_exact(ser, length) if length else b""
            frames.append((frame_type, payload))
    except EOFError:
        pass
    return frames


def append_frame(buf, frame_type, payload):
    payload = payload.encode() if isinstance(payload, str) else payload
    buf += bytes([FRAME_MAGIC, ord(frame_type)])
    buf += struct.pack(">I", len(payload))
    buf += payload
    return buf


stream = bytes([0x01, 0x02, 0x03, 0x00, 0x7F])
stream = append_frame(stream, "A", "first-audio-frame-payload")
stream += bytes([FRAME_MAGIC, ord("Z"), 0, 0, 0, 5, 0x11, 0x22, 0x33, 0x44, 0x55])
stream = append_frame(stream, "C", "123|ENCODE_US:avg=11000,max=15000,n=50,core=1")
bad_len = MAX_FRAME_LEN + 100
stream += bytes([FRAME_MAGIC, ord("A")]) + struct.pack(">I", bad_len)
stream = append_frame(stream, "A", "second-audio-frame-payload-after-recovery")

frames = parse_stream(MockSerial(stream))
print(f"parsed {len(frames)} frames:")
for ftype, payload in frames:
    print(f"  type={ftype} len={len(payload)} payload={payload.decode()}")
