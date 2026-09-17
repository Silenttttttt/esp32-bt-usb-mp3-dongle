"""Builds and serves a growing FAT12 volume with one file, using the exact
same design the real firmware uses (and that Stage 1 already proved works
on real hardware): declare the file's full size up front (FAT drivers cache
size at mount time and never re-poll it), pre-chain the FAT for the whole
declared capacity, and back the data region with a live buffer that's still
being filled -- unwritten sectors read back as zero.

This module is the thing meant to be ported near-verbatim into the real S3
firmware's onRead()/onWrite() callbacks (see ../firmware/ for the Digispark
C version this was originally validated against).
"""

import os

SECTOR_SIZE = 512
SECTORS_PER_CLUSTER = 8  # 4KB clusters
RESERVED_SECTORS = 1
NUM_FATS = 2
ROOT_ENTRIES = 16
FILE_NAME = "STREAM  MP3"  # 8.3, space-padded: "STREAM" + "MP3"

# A few seconds of pre-encoded silence (mono/44100Hz/128kbps, matching the
# ESP32's Shine encoder config), written into the ring the instant the disk
# is created -- before any real audio has arrived. Without this, the very
# first mount ever sees pure zero bytes at the start of the file (nothing
# "not encoded yet"), and real audio only starts filling the ring once the
# phone actually starts sending it, ~7s after BT connects at the earliest.
# Priming with real (if silent) MP3 frames up front means the radio's own
# decoder has valid bytes to engage with immediately on mount, instead of
# sitting through however long real content takes to physically arrive
# before it ever gets ANYTHING to decode.
SILENCE_PRIMER_PATH = os.path.join(os.path.dirname(__file__), "silence_primer.mp3")


def _pack_fat12(entries):
    out = bytearray()
    i = 0
    while i < len(entries):
        e0 = entries[i]
        e1 = entries[i + 1] if i + 1 < len(entries) else 0
        out.append(e0 & 0xFF)
        out.append(((e0 >> 8) & 0x0F) | ((e1 & 0x0F) << 4))
        out.append((e1 >> 4) & 0xFF)
        i += 2
    return bytes(out)


class GrowingFat12Disk:
    """A FAT12 volume whose one file's data region is backed by a RING
    buffer of exactly `capacity_bytes`. Callers append bytes via `.append()`
    as they're produced (e.g. by a live MP3 encoder); once the ring fills,
    new audio overwrites the oldest audio in place.

    This matters for a real, unattended, hours-long drive: a real car
    radio's cheap USB-MP3 firmware just reads "the file" sequentially from
    its start every time -- it has no notion of "resume near the live
    position." With an ever-growing file, "the start of the file" gets
    further behind live for the entire session, and pausing/resuming on the
    source has no effect on what's already queued up to play (confirmed by
    direct testing, not theory: a real phone paused mid-song while the
    radio kept playing from a stale backlog). With a bounded ring, "the
    start of the file" is never more than one ring's worth of audio behind
    live -- capacity_bytes should be sized as the acceptable worst-case
    catch-up lag (e.g. a handful of seconds), not as "how long is the
    drive."""

    def __init__(self, capacity_bytes=8 * 1024 * 1024):
        self.cluster_size = SECTORS_PER_CLUSTER * SECTOR_SIZE
        self.data_clusters = capacity_bytes // self.cluster_size
        assert self.data_clusters < 4085, "must stay FAT12, not FAT16"

        fat_entries_needed = self.data_clusters + 2  # clusters 0,1 reserved
        fat_bytes = -(-fat_entries_needed * 3 // 2)  # ceil(entries * 1.5)
        self.fat_sectors = -(-fat_bytes // SECTOR_SIZE)

        self.root_dir_sectors = (ROOT_ENTRIES * 32) // SECTOR_SIZE
        self.first_data_lba = RESERVED_SECTORS + NUM_FATS * self.fat_sectors + self.root_dir_sectors
        self.total_sectors = self.first_data_lba + self.data_clusters * SECTORS_PER_CLUSTER
        self.declared_file_size = self.data_clusters * self.cluster_size

        self.write_pos = 0
        self.total_written = 0
        # Ring-relative offset of the most recent cluster the reader has
        # actually been served (updated by read_sectors() on every
        # successful -- torn or not -- read in the data region). Used by
        # append()'s unread_protect option to stop the writer from ever
        # overwriting content the reader hasn't reached yet in its current
        # lap around the ring -- see append()'s docstring for the real
        # incident this fixes.
        self.last_read_offset = 0

        # Fixed-size ring, pre-zeroed. Unlike the old ever-growing bytearray,
        # this is exactly `declared_file_size` bytes from the start; append()
        # overwrites in place once it wraps.
        self.buffer = bytearray(self.declared_file_size)

        self._boot_sector = self._build_boot_sector()
        self._fat_sector_cache = self._build_fat()
        self._root_dir_sector = self._build_root_dir()

        try:
            with open(SILENCE_PRIMER_PATH, "rb") as f:
                self.append(f.read())
        except FileNotFoundError:
            pass  # priming is a latency nicety, not a correctness requirement

    def append(self, data, unread_protect=False):
        """Write new audio bytes into the ring, overwriting the oldest
        bytes once capacity is reached.

        Real bug found via live-phone testing: on a short ring, a writer
        that keeps producing content at a steady rate regardless of
        real-world disruption (e.g. injecting silence during a pause, so
        the ring never freezes -- see the ESP32 firmware's
        feed_silence_if_no_real_audio()) has no protection at all against
        catching up to and overwriting content the reader hasn't reached
        yet in its current lap, if the READER side ever genuinely stalls
        for any reason (a stuck TCP connection, a crashed car_sim.py, a
        slow retry storm) for close to a full ring-duration. The existing
        avoid_straddle/margin mechanism in read_sectors() only protects the
        READER from reading too close to the live write edge -- it has no
        equivalent protection in the other direction. `unread_protect=True`
        adds that missing half: refuses (returns False) a write that would
        land exactly on the ring-offset the reader most recently reached,
        instead of silently overwriting unread content. Callers should
        retry shortly (same bounded-retry style already used on the read
        side) rather than treat a False return as fatal -- see
        s3_sim_serial.py's receive_from_esp32() for the retry loop this
        pairs with. Defaults to False (original, always-succeeds behavior)
        so nothing else in this class changes unless a caller opts in."""
        cap = self.declared_file_size
        n = len(data)
        if n >= cap:
            if unread_protect:
                return False  # can't safely protect an overwrite-everything write
            # a single write bigger than the whole ring -- keep only its tail
            self.buffer[:] = data[-cap:]
            self.write_pos = 0
            self.total_written += n
            return True
        end = self.write_pos + n
        if unread_protect and self.total_written >= cap:
            # Nothing to protect before the ring has wrapped at least once
            # -- every byte is either legitimate fresh content from this
            # same first pass, or not-yet-encoded zero-fill, neither of
            # which the reader could have "already consumed" in any
            # meaningful sense yet.
            wrapped_end = end % cap if end > cap else end
            if end <= cap:
                unsafe = self.write_pos <= self.last_read_offset < end
            else:
                unsafe = (self.last_read_offset >= self.write_pos
                          or self.last_read_offset < wrapped_end)
            if unsafe:
                return False
        if end <= cap:
            self.buffer[self.write_pos:end] = data
        else:
            first_part = cap - self.write_pos
            self.buffer[self.write_pos:cap] = data[:first_part]
            self.buffer[0:end - cap] = data[first_part:]
        self.write_pos = end % cap
        self.total_written += n
        return True

    def valid_bytes(self):
        """How much of the ring currently holds real (not zero-padded)
        audio -- caps at declared_file_size once the ring has wrapped at
        least once, since every position then holds *some* real audio."""
        return min(self.total_written, self.declared_file_size)

    def _build_boot_sector(self):
        bs = bytearray(SECTOR_SIZE)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = b"BOARDSIM"
        bs[11:13] = SECTOR_SIZE.to_bytes(2, "little")
        bs[13] = SECTORS_PER_CLUSTER
        bs[14:16] = RESERVED_SECTORS.to_bytes(2, "little")
        bs[16] = NUM_FATS
        bs[17:19] = ROOT_ENTRIES.to_bytes(2, "little")
        total16 = self.total_sectors if self.total_sectors < 0x10000 else 0
        bs[19:21] = total16.to_bytes(2, "little")
        bs[21] = 0xF8
        bs[22:24] = self.fat_sectors.to_bytes(2, "little")
        bs[24:26] = (32).to_bytes(2, "little")
        bs[26:28] = (64).to_bytes(2, "little")
        bs[28:32] = (0).to_bytes(4, "little")
        bs[32:36] = (self.total_sectors if total16 == 0 else 0).to_bytes(4, "little")
        bs[36] = 0x80
        bs[38] = 0x29
        bs[39:43] = (0xC0FFEE00).to_bytes(4, "little")
        bs[43:54] = b"BOARDSIM   "
        bs[54:62] = b"FAT12   "
        bs[510] = 0x55
        bs[511] = 0xAA
        return bytes(bs)

    def _build_fat(self):
        entries = [0] * (self.data_clusters + 2)
        entries[0] = 0xFF8
        entries[1] = 0xFFF
        # the one file occupies every data cluster in order, 2..N, chained
        for c in range(2, self.data_clusters + 2):
            entries[c] = c + 1 if c < self.data_clusters + 1 else 0xFFF
        packed = _pack_fat12(entries)
        sector = bytearray(self.fat_sectors * SECTOR_SIZE)
        sector[: len(packed)] = packed
        return bytes(sector)

    def _build_root_dir(self):
        sector = bytearray(self.root_dir_sectors * SECTOR_SIZE)
        entry = bytearray(32)
        entry[0:11] = FILE_NAME.encode("ascii")
        entry[11] = 0x20  # ARCHIVE
        entry[16:18] = (0x4A21).to_bytes(2, "little")
        entry[18:20] = (0x4A21).to_bytes(2, "little")
        entry[20:22] = (0).to_bytes(2, "little")
        entry[24:26] = (0x4A21).to_bytes(2, "little")
        entry[26:28] = (2).to_bytes(2, "little")  # first cluster = 2
        entry[28:32] = self.declared_file_size.to_bytes(4, "little")
        sector[0:32] = entry
        return bytes(sector)

    def read_sectors(self, lba, count, avoid_straddle=True, margin=0):
        """The one call the SCSI READ10 handler makes. Mirrors exactly what
        the real onRead(lba, offset, buffer, size) callback would do.

        Returns None instead of bytes when avoid_straddle is True and
        write_pos hasn't yet moved at least `margin` bytes past the end of
        the requested range -- confirmed, real bug: a reader's read is one
        whole cluster (4096 bytes here), but a writer's atomic unit is a
        single BT-audio frame (a few hundred bytes), so a read landing on
        write_pos can splice together bytes from two different points in
        time (everything before write_pos is this lap's fresh audio,
        everything after is still last lap's) into one "atomic" transfer --
        an in-stream jump backward in time that plays as an audible glitch.
        The lock alone doesn't prevent this: it only stops a read/write
        from tearing a single call, not a read from spanning many small
        writes that already landed earlier.

        margin=0 only guarantees write_pos isn't CURRENTLY inside the
        range -- confirmed, also real: with zero margin, the reader ends
        up trailing the writer by exactly one cluster-width forever (both
        march the ring at the same average rate, so whatever gap exists at
        connect time is roughly preserved), which means it's constantly
        right at the writer's live edge. Direct measurement: ~90%+ of
        reads needed 40-58 retries (~230-290ms) as a STEADY STATE, not an
        occasional spike -- and any small timing jitter on top of that
        baseline pushed several consecutive reads to their retry ceiling,
        producing real, reproducible ~3.5s hard-mute periods every ~11s
        (confirmed via a 99-agent forensic pass on a real recording,
        cross-referenced against live /proc sampling that caught the
        reader genuinely blocked, not the upstream BT/encode/serial path).
        A nonzero margin institutionalizes real breathing room: the writer
        must clear a few cluster-widths past the read before it's served,
        which self-corrects continuously (the retry loop just waits
        longer whenever jitter narrows the gap) rather than depending on
        a one-time initial condition that jitter can erode back to zero.
        Caller should retry shortly instead of serving a torn read; pass
        avoid_straddle=False to force a read anyway (bounded last resort).
        """
        if avoid_straddle and lba >= self.first_data_lba:
            cap = self.declared_file_size
            first_data_off = (lba - self.first_data_lba) * SECTOR_SIZE
            length = count * SECTOR_SIZE
            unsafe_end = first_data_off + length + margin
            if unsafe_end <= cap:
                unsafe = first_data_off <= self.write_pos < unsafe_end
            else:
                # margin pushed the unsafe zone past the ring's end -- it
                # wraps, so treat it as two segments instead of one.
                unsafe = self.write_pos >= first_data_off or self.write_pos < unsafe_end - cap
            if unsafe:
                return None

        out = bytearray(count * SECTOR_SIZE)
        for i in range(count):
            cur_lba = lba + i
            sector_off = i * SECTOR_SIZE
            if cur_lba == 0:
                out[sector_off:sector_off + SECTOR_SIZE] = self._boot_sector
            elif cur_lba < RESERVED_SECTORS + NUM_FATS * self.fat_sectors:
                fat_rel = (cur_lba - RESERVED_SECTORS) % self.fat_sectors
                out[sector_off:sector_off + SECTOR_SIZE] = (
                    self._fat_sector_cache[fat_rel * SECTOR_SIZE:(fat_rel + 1) * SECTOR_SIZE]
                )
            elif cur_lba < self.first_data_lba:
                dir_rel = cur_lba - (RESERVED_SECTORS + NUM_FATS * self.fat_sectors)
                out[sector_off:sector_off + SECTOR_SIZE] = (
                    self._root_dir_sector[dir_rel * SECTOR_SIZE:(dir_rel + 1) * SECTOR_SIZE]
                )
            else:
                data_off = (cur_lba - self.first_data_lba) * SECTOR_SIZE
                avail = self.valid_bytes()
                if data_off < avail:
                    end = min(data_off + SECTOR_SIZE, self.declared_file_size)
                    real = bytes(self.buffer[data_off:end])
                    out[sector_off:sector_off + len(real)] = real
                # else: stays zero -- not encoded yet, exactly like real HW
                # Record how far the reader has actually reached, for
                # append()'s unread_protect check -- see its docstring.
                # Updated for every data-region sector served (torn or
                # not): even a torn read means the reader really did move
                # past this position, so the writer still shouldn't
                # immediately overwrite it again.
                self.last_read_offset = (data_off + SECTOR_SIZE) % self.declared_file_size
        return bytes(out)
