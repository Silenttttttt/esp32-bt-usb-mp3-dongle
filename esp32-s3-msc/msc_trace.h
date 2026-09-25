// MSC_TRACE (2026-09-25, car capture session): log exactly what the USB host
// (the Kenwood) does to the S3 -- every SCSI command with its CDB, result
// status, bytes returned and timing, plus bus resets, descriptor requests,
// class control requests and endpoint stalls -- on the debug serial port.
//
// How: the Arduino USBMSC class answers INQUIRY/READ CAPACITY/MODE SENSE/...
// inside TinyUSB, out of reach of the sketch's callbacks. So the link step
// wraps TinyUSB's own internal calls (GNU ld --wrap): the MSC driver's
// transfer-complete callback (sees every 31-byte CBW the host sends) and
// its usbd_edpt_xfer calls (sees every data-IN block and the 13-byte CSW
// with the status). That covers every command, whoever handles it.
//
// Build: add -DMSC_TRACE to F and the --wrap list below as the ELF link flags
// (CLAUDE.md has the full command). A missing half fails the link loudly
// (__real_*/__wrap_* undefined), so it can't be silently half-on.
//   compiler.c.elf.extra_flags=-Wl,--wrap=usbd_edpt_xfer -Wl,--wrap=usbd_edpt_stall
//     -Wl,--wrap=mscd_xfer_cb -Wl,--wrap=mscd_reset -Wl,--wrap=mscd_control_xfer_cb
//     -Wl,--wrap=tud_descriptor_device_cb -Wl,--wrap=tud_descriptor_configuration_cb
//     -Wl,--wrap=tud_descriptor_string_cb
//
// Records go into a PSRAM ring from the USB task (a few hundred ns each, no
// printing there -- a blocked Serial write would stall USB and change what
// the radio sees); a low-priority task prints them. Serial runs at
// MSC_TRACE_BAUD. Runs of back-to-back sequential READ10s are merged into
// one line. A "DROPPED n" line means the ring overflowed.
//
// Line format (device time in ms since boot first):
//   T <ms> CMD <name> <args> st=<0 ok|1 fail|2 phase> in=<bytes> res=<residue> dt=<us> [data=<hex>]
//   T <ms> R10 <region> lba=<lba> n=<sectors> [xK runs] ... lag=<bytes behind live>
//   T <ms> BUS/DESC/CTRL/STALL/EV ...
// Included twice by the .ino: before fat_disk_shared.h (event codes and the
// FATDISK_TRACE hook it calls) and after it (the rest, which needs the disk
// layout and ring globals).
#ifdef MSC_TRACE
#ifndef MSC_TRACE_DECLS
#define MSC_TRACE_DECLS
// App-level events (file switches, early ends, titles) share the timeline.
enum : uint32_t {
  EV_USB_STARTED = 1, EV_USB_STOPPED, EV_USB_SUSPEND, EV_USB_RESUME,
  EV_ANCHOR, EV_SWITCH, EV_RELAY, EV_FORCE_END, EV_RESTORE_END, EV_TITLE, EV_BOOT,
};
static void trace_event(uint32_t code, uint32_t a, uint32_t b, uint32_t c);
#define FATDISK_TRACE(ev, a, b, c) trace_event(EV_##ev, (a), (b), (c))
#endif  // MSC_TRACE_DECLS
#endif  // MSC_TRACE

#if defined(MSC_TRACE) && defined(FAT_DISK_SHARED_H) && !defined(MSC_TRACE_H)
#define MSC_TRACE_H

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#ifndef MSC_TRACE_BAUD
#define MSC_TRACE_BAUD 921600
#endif
static const uint32_t TRACE_CAP = 16384;  // records (~0.8MB of PSRAM)

enum : uint8_t { TR_CMD = 1, TR_BUSRESET, TR_DESC, TR_CTRL, TR_STALL, TR_EVENT, TR_BADCBW };

struct TraceRec {
  uint64_t t_us;       // start (CBW arrival) for commands
  uint32_t dt_us;      // CBW -> CSW
  uint8_t type;
  uint8_t status;      // CSW status
  uint8_t cdb_len;
  uint8_t preview_len;
  uint8_t cdb[12];
  uint32_t xfer_len;   // host's dCBWDataTransferLength
  uint32_t in_bytes;   // data actually queued to the host
  uint32_t residue;
  uint32_t a, b, c;    // type-specific
  uint8_t preview[24]; // first bytes of the data-IN phase (non-READ10)
};

static TraceRec *g_tr = nullptr;
static volatile uint32_t g_tr_head = 0, g_tr_tail = 0, g_tr_dropped = 0;
// The first BOOT_KEEP records since boot are also kept for good, so a
// cold-boot capture works with no laptop attached (S3 powered by the radio
// alone, as installed): attach later and send 'B' to replay them.
static const uint32_t BOOT_KEEP = 4096;
static TraceRec *g_tr_boot = nullptr;
static volatile uint32_t g_tr_boot_n = 0;
static portMUX_TYPE g_tr_mux = portMUX_INITIALIZER_UNLOCKED;

static void trace_push(const TraceRec &r) {
  if (!g_tr) return;
  portENTER_CRITICAL_SAFE(&g_tr_mux);
  uint32_t h = g_tr_head;
  if (h - g_tr_tail >= TRACE_CAP) {
    g_tr_dropped++;
  } else {
    g_tr[h % TRACE_CAP] = r;
    g_tr_head = h + 1;
  }
  if (g_tr_boot && g_tr_boot_n < BOOT_KEEP) g_tr_boot[g_tr_boot_n++] = r;
  portEXIT_CRITICAL_SAFE(&g_tr_mux);
}

static void trace_event(uint32_t code, uint32_t a, uint32_t b, uint32_t c) {
  TraceRec r = {};
  r.t_us = esp_timer_get_time();
  r.type = TR_EVENT;
  r.a = code; r.b = a; r.c = b; r.xfer_len = c;
  trace_push(r);
}

// ---- TinyUSB wraps (all run in the TinyUSB task) ----------------------------
#include "tusb.h"
extern "C" {
bool __real_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes, bool is_isr);
bool __real_usbd_edpt_stall(uint8_t rhport, uint8_t ep_addr);
bool __real_mscd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes);
void __real_mscd_reset(uint8_t rhport);
bool __real_mscd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req);
uint8_t const *__real_tud_descriptor_device_cb(void);
uint8_t const *__real_tud_descriptor_configuration_cb(uint8_t index);
uint16_t const *__real_tud_descriptor_string_cb(uint8_t index, uint16_t langid);
}

static uint8_t *g_tr_out_buf = nullptr;  // buffer of the last OUT transfer queued (the CBW slot)
static bool g_tr_cmd_open = false;
static TraceRec g_tr_cur;

extern "C" bool __wrap_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes, bool is_isr) {
  if (!(ep_addr & 0x80)) {
    g_tr_out_buf = buffer;
  } else if (total_bytes == 13 && buffer && buffer[0] == 'U' && buffer[1] == 'S' && buffer[2] == 'B' && buffer[3] == 'S') {
    if (g_tr_cmd_open) {  // CSW: the command is done
      g_tr_cur.dt_us = (uint32_t)(esp_timer_get_time() - g_tr_cur.t_us);
      g_tr_cur.residue = (uint32_t)buffer[8] | ((uint32_t)buffer[9] << 8) | ((uint32_t)buffer[10] << 16) | ((uint32_t)buffer[11] << 24);
      g_tr_cur.status = buffer[12];
#ifdef FATDISK_ALWAYS_SERVE_LIVE
      g_tr_cur.a = g_write_pos;
      g_tr_cur.b = g_live_read_cursor;
#else
      g_tr_cur.a = g_write_pos;
      g_tr_cur.b = 0;
#endif
      trace_push(g_tr_cur);
      g_tr_cmd_open = false;
    }
  } else if (g_tr_cmd_open) {  // data-IN phase
    if (g_tr_cur.in_bytes == 0 && buffer) {
      uint32_t n = total_bytes < sizeof(g_tr_cur.preview) ? total_bytes : sizeof(g_tr_cur.preview);
      memcpy(g_tr_cur.preview, buffer, n);
      g_tr_cur.preview_len = (uint8_t)n;
    }
    g_tr_cur.in_bytes += total_bytes;
  }
  return __real_usbd_edpt_xfer(rhport, ep_addr, buffer, total_bytes, is_isr);
}

extern "C" bool __wrap_mscd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t event, uint32_t xferred_bytes) {
  if (!(ep_addr & 0x80) && event == XFER_RESULT_SUCCESS && g_tr_out_buf && !g_tr_cmd_open) {
    const uint8_t *p = g_tr_out_buf;
    if (xferred_bytes == 31 && p[0] == 'U' && p[1] == 'S' && p[2] == 'B' && p[3] == 'C') {
      memset(&g_tr_cur, 0, sizeof(g_tr_cur));
      g_tr_cur.t_us = esp_timer_get_time();
      g_tr_cur.type = TR_CMD;
      g_tr_cur.xfer_len = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
      g_tr_cur.c = p[12];  // bmCBWFlags (0x80 = data IN)
      uint8_t n = p[14] & 0x1F;
      g_tr_cur.cdb_len = n;
      memcpy(g_tr_cur.cdb, p + 15, n < 12 ? n : 12);
      g_tr_cmd_open = true;
    } else if (xferred_bytes != 31 || p[0] != 'U') {
      // Only log a bad CBW when no command is open (then this OUT was meant
      // to be a CBW); data-OUT phases (WRITE10) arrive while one is open.
      TraceRec r = {};
      r.t_us = esp_timer_get_time();
      r.type = TR_BADCBW;
      r.a = xferred_bytes;
      trace_push(r);
    }
  }
  return __real_mscd_xfer_cb(rhport, ep_addr, event, xferred_bytes);
}

extern "C" bool __wrap_usbd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
  TraceRec r = {};
  r.t_us = esp_timer_get_time();
  r.type = TR_STALL;
  r.a = ep_addr;
  r.b = g_tr_cmd_open ? g_tr_cur.cdb[0] : 0xFFFF;
  trace_push(r);
  return __real_usbd_edpt_stall(rhport, ep_addr);
}

extern "C" void __wrap_mscd_reset(uint8_t rhport) {
  TraceRec r = {};
  r.t_us = esp_timer_get_time();
  r.type = TR_BUSRESET;
  trace_push(r);
  g_tr_cmd_open = false;
  __real_mscd_reset(rhport);
}

extern "C" bool __wrap_mscd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req) {
  if (stage == CONTROL_STAGE_SETUP && req) {
    TraceRec r = {};
    r.t_us = esp_timer_get_time();
    r.type = TR_CTRL;
    r.a = ((uint32_t)req->bmRequestType << 8) | req->bRequest;
    r.b = req->wValue;
    r.c = ((uint32_t)req->wIndex << 16) | req->wLength;
    trace_push(r);
  }
  return __real_mscd_control_xfer_cb(rhport, stage, req);
}

static void trace_desc(uint32_t kind, uint32_t idx, uint32_t lang) {
  TraceRec r = {};
  r.t_us = esp_timer_get_time();
  r.type = TR_DESC;
  r.a = kind; r.b = idx; r.c = lang;
  trace_push(r);
}
extern "C" uint8_t const *__wrap_tud_descriptor_device_cb(void) {
  trace_desc(1, 0, 0);
  return __real_tud_descriptor_device_cb();
}
extern "C" uint8_t const *__wrap_tud_descriptor_configuration_cb(uint8_t index) {
  trace_desc(2, index, 0);
  return __real_tud_descriptor_configuration_cb(index);
}
extern "C" uint16_t const *__wrap_tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  trace_desc(3, index, langid);
  return __real_tud_descriptor_string_cb(index, langid);
}

// ---- printing -------------------------------------------------------------

static const char *scsi_name(uint8_t op) {
  switch (op) {
    case 0x00: return "TEST_UNIT_READY";
    case 0x03: return "REQUEST_SENSE";
    case 0x12: return "INQUIRY";
    case 0x1A: return "MODE_SENSE6";
    case 0x1B: return "START_STOP";
    case 0x1E: return "PREVENT_ALLOW";
    case 0x23: return "READ_FORMAT_CAPACITIES";
    case 0x25: return "READ_CAPACITY10";
    case 0x28: return "READ10";
    case 0x2A: return "WRITE10";
    case 0x2F: return "VERIFY10";
    case 0x35: return "SYNC_CACHE10";
    case 0x5A: return "MODE_SENSE10";
    case 0x88: return "READ16";
    case 0x9E: return "SERVICE_ACTION_IN16";
    case 0xA0: return "REPORT_LUNS";
    case 0xA8: return "READ12";
    default: return "UNKNOWN";
  }
}

// Where an LBA lands on this volume, e.g. "boot", "fat1+3", "root+0", "f1+0x1a000".
static void region_str(uint32_t lba, char *out, size_t cap) {
  const uint32_t fat1 = RESERVED_SECTORS, fat2 = fat1 + FAT_SECTORS, root = fat2 + FAT_SECTORS;
  if (lba < fat1) snprintf(out, cap, "boot");
  else if (lba < fat2) snprintf(out, cap, "fat1+%lu", (unsigned long)(lba - fat1));
  else if (lba < root) snprintf(out, cap, "fat2+%lu", (unsigned long)(lba - fat2));
  else if (lba < FIRST_DATA_LBA) snprintf(out, cap, "root+%lu", (unsigned long)(lba - root));
  else if (lba < TOTAL_SECTORS) {
    uint32_t off = (lba - FIRST_DATA_LBA) * SECTOR_SIZE;
    snprintf(out, cap, "f%lu+0x%lx", (unsigned long)(off / DECLARED_FILE_SIZE), (unsigned long)(off % DECLARED_FILE_SIZE));
  } else snprintf(out, cap, "PAST_END");
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Bytes the served data sat behind the live write edge (ring distance).
static uint32_t live_lag(const TraceRec &r) {
  return (r.a + DECLARED_FILE_SIZE - (r.b % DECLARED_FILE_SIZE)) % DECLARED_FILE_SIZE;
}

// Pending merged READ10 run.
static bool g_run_open = false;
static TraceRec g_run_first;
static uint32_t g_run_next_lba = 0, g_run_count = 0, g_run_sectors = 0, g_run_max_dt = 0, g_run_len = 0;
static uint64_t g_run_last_end_us = 0;

static void flush_run() {
  if (!g_run_open) return;
  g_run_open = false;
  char reg[32];
  uint32_t lba = be32(g_run_first.cdb + 2);
  region_str(lba, reg, sizeof(reg));
  uint32_t span_ms = (uint32_t)((g_run_last_end_us - g_run_first.t_us) / 1000);
  if (g_run_count == 1) {
    Serial.printf("T %lu R10 %s lba=%lu n=%lu dt=%luus lag=%lu\n", (unsigned long)(g_run_first.t_us / 1000), reg,
                  (unsigned long)lba, (unsigned long)g_run_sectors, (unsigned long)g_run_max_dt,
                  (unsigned long)live_lag(g_run_first));
  } else {
    Serial.printf("T %lu R10 %s lba=%lu n=%lu x%lu (%lu sect total, %lums, max_dt=%luus) lag=%lu\n",
                  (unsigned long)(g_run_first.t_us / 1000), reg, (unsigned long)lba, (unsigned long)g_run_len,
                  (unsigned long)g_run_count, (unsigned long)g_run_sectors, (unsigned long)span_ms,
                  (unsigned long)g_run_max_dt, (unsigned long)live_lag(g_run_first));
  }
}

static void print_rec(const TraceRec &r) {
  unsigned long ms = (unsigned long)(r.t_us / 1000);
  char hex[3 * 24 + 1];
  switch (r.type) {
    case TR_CMD: {
      uint8_t op = r.cdb[0];
      if (op == 0x28 && r.status == 0) {
        uint32_t lba = be32(r.cdb + 2);
        uint32_t n = ((uint32_t)r.cdb[7] << 8) | r.cdb[8];
        // Merge back-to-back sequential reads of the same size (<30ms apart).
        if (g_run_open && lba == g_run_next_lba && n == g_run_len && r.t_us - g_run_last_end_us < 30000) {
          g_run_count++; g_run_sectors += n; g_run_next_lba = lba + n;
          g_run_last_end_us = r.t_us + r.dt_us;
          if (r.dt_us > g_run_max_dt) g_run_max_dt = r.dt_us;
          return;
        }
        flush_run();
        g_run_open = true; g_run_first = r; g_run_next_lba = lba + n; g_run_count = 1;
        g_run_sectors = n; g_run_len = n; g_run_max_dt = r.dt_us; g_run_last_end_us = r.t_us + r.dt_us;
        return;
      }
      flush_run();
      char args[64] = "";
      if (op == 0x28 || op == 0x2A || op == 0x2F) {
        char reg[32];
        uint32_t lba = be32(r.cdb + 2);
        region_str(lba, reg, sizeof(reg));
        snprintf(args, sizeof(args), "%s lba=%lu n=%u", reg, (unsigned long)lba, ((unsigned)r.cdb[7] << 8) | r.cdb[8]);
      } else if (op == 0x12) {
        snprintf(args, sizeof(args), "evpd=%u page=0x%02x alloc=%u", r.cdb[1] & 1, r.cdb[2], ((unsigned)r.cdb[3] << 8) | r.cdb[4]);
      } else if (op == 0x1A) {
        snprintf(args, sizeof(args), "page=0x%02x alloc=%u", r.cdb[2], r.cdb[4]);
      } else if (op == 0x03) {
        snprintf(args, sizeof(args), "alloc=%u", r.cdb[4]);
      } else if (op == 0x1B) {
        snprintf(args, sizeof(args), "start=%u loej=%u", r.cdb[4] & 1, (r.cdb[4] >> 1) & 1);
      } else if (op == 0x1E) {
        snprintf(args, sizeof(args), "prevent=%u", r.cdb[4] & 3);
      }
      Serial.printf("T %lu CMD %s %s st=%u in=%lu/%lu res=%lu dt=%luus", ms, scsi_name(op), args, r.status,
                    (unsigned long)r.in_bytes, (unsigned long)r.xfer_len, (unsigned long)r.residue,
                    (unsigned long)r.dt_us);
      if (op == 0x28) Serial.printf(" lag=%lu", (unsigned long)live_lag(r));
      if (!strcmp(scsi_name(op), "UNKNOWN") || op == 0x5A || op == 0x9E || op == 0xA0) {
        size_t k = 0;
        for (uint8_t i = 0; i < r.cdb_len && i < 12; i++) k += snprintf(hex + k, sizeof(hex) - k, "%02x", r.cdb[i]);
        Serial.printf(" cdb=%s", hex);
      }
      if (op != 0x28 && r.preview_len) {
        size_t k = 0;
        for (uint8_t i = 0; i < r.preview_len; i++) k += snprintf(hex + k, sizeof(hex) - k, "%02x", r.preview[i]);
        Serial.printf(" data=%s", hex);
      }
      Serial.print("\n");
      break;
    }
    case TR_BUSRESET: flush_run(); Serial.printf("T %lu BUS mscd_reset (bus reset / deconfigure)\n", ms); break;
    case TR_DESC: {
      flush_run();
      static const char *k[] = {"?", "device", "config", "string"};
      Serial.printf("T %lu DESC %s idx=%lu lang=0x%04lx\n", ms, k[r.a < 4 ? r.a : 0], (unsigned long)r.b, (unsigned long)r.c);
      break;
    }
    case TR_CTRL: {
      flush_run();
      uint8_t req = r.a & 0xFF;
      const char *nm = req == 0xFE ? "GET_MAX_LUN" : req == 0xFF ? "BOT_RESET"
                     : (req == 0x01 && (r.a >> 8) == 0x02) ? "CLEAR_HALT" : "?";
      Serial.printf("T %lu CTRL %s bmRT=0x%02lx bReq=0x%02x wValue=%lu wIndex=%lu wLength=%lu\n", ms, nm,
                    (unsigned long)(r.a >> 8), (unsigned)req, (unsigned long)r.b, (unsigned long)(r.c >> 16),
                    (unsigned long)(r.c & 0xFFFF));
      break;
    }
    case TR_STALL:
      flush_run();
      Serial.printf("T %lu STALL ep=0x%02lx during_op=0x%02lx\n", ms, (unsigned long)r.a, (unsigned long)r.b);
      break;
    case TR_BADCBW: flush_run(); Serial.printf("T %lu BADCBW len=%lu\n", ms, (unsigned long)r.a); break;
    case TR_EVENT: {
      flush_run();
      uint32_t a = r.b, b = r.c, c = r.xfer_len;
      switch (r.a) {
        case EV_USB_STARTED: Serial.printf("T %lu EV usb_started (host configured us)\n", ms); break;
        case EV_USB_STOPPED: Serial.printf("T %lu EV usb_stopped\n", ms); break;
        case EV_USB_SUSPEND: Serial.printf("T %lu EV usb_suspend\n", ms); break;
        case EV_USB_RESUME: Serial.printf("T %lu EV usb_resume\n", ms); break;
        case EV_ANCHOR: Serial.printf("T %lu EV anchor file=%lu\n", ms, (unsigned long)a); break;
        case EV_SWITCH:
          Serial.printf("T %lu EV switch %lu->%lu flags=%s%s%s read_end=0x%lx\n", ms, (unsigned long)(a >> 8),
                        (unsigned long)(a & 0xFF), (b & 1) ? "natural_eof " : "", (b & 2) ? "suppressed " : "",
                        (b & 4) ? "RELAYED" : "", (unsigned long)c);
          break;
        case EV_RELAY: Serial.printf("T %lu EV RADIO_CMD:%s sent\n", ms, a ? "next" : "prev"); break;
        case EV_FORCE_END: Serial.printf("T %lu EV early_end file=%lu size=0x%lx (was 0x%lx)\n", ms, (unsigned long)a, (unsigned long)b, (unsigned long)c); break;
        case EV_RESTORE_END: Serial.printf("T %lu EV early_end_restored file=%lu size=0x%lx\n", ms, (unsigned long)a, (unsigned long)b); break;
        case EV_TITLE: Serial.printf("T %lu EV title changed=%lu len=%lu\n", ms, (unsigned long)a, (unsigned long)b); break;
        case EV_BOOT: Serial.printf("T %lu EV trace_on cap=%lu\n", ms, (unsigned long)a); break;
        default: Serial.printf("T %lu EV code=%lu %lu %lu %lu\n", ms, (unsigned long)r.a, (unsigned long)a, (unsigned long)b, (unsigned long)c); break;
      }
      break;
    }
  }
}

static void replay_boot_records() {
  flush_run();
  uint32_t n = g_tr_boot_n;
  Serial.printf("=== REPLAY %lu boot records (from power-on) ===\n", (unsigned long)n);
  for (uint32_t i = 0; i < n; i++) print_rec(g_tr_boot[i]);
  flush_run();
  Serial.printf("=== REPLAY end%s ===\n", n >= BOOT_KEEP ? " (buffer full, later records not kept)" : "");
}

static void trace_task(void *) {
  uint32_t last_dropped = 0;
  while (true) {
    bool any = false;
    while (Serial.available() > 0) {
      if (Serial.read() == 'B') replay_boot_records();
    }
    while (g_tr_tail != g_tr_head) {
      TraceRec r = g_tr[g_tr_tail % TRACE_CAP];
      portENTER_CRITICAL(&g_tr_mux);
      g_tr_tail++;
      portEXIT_CRITICAL(&g_tr_mux);
      print_rec(r);
      any = true;
    }
    // Close a pending READ10 run once it's gone quiet.
    if (!any && g_run_open && (uint64_t)esp_timer_get_time() - g_run_last_end_us > 50000) flush_run();
    if (g_tr_dropped != last_dropped) {
      Serial.printf("T %lu DROPPED %lu trace records (ring full)\n", (unsigned long)(esp_timer_get_time() / 1000),
                    (unsigned long)(g_tr_dropped - last_dropped));
      last_dropped = g_tr_dropped;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Call before USB.begin().
static void msc_trace_begin() {
  // Without the --wrap flags the unused __wrap_* functions are garbage
  // collected and the build silently traces nothing; taking __real_*'s
  // address here makes it fail the link instead.
  static void *volatile s_need_wrap_flags = (void *)&__real_mscd_xfer_cb;
  (void)s_need_wrap_flags;
  g_tr = (TraceRec *)heap_caps_malloc(TRACE_CAP * sizeof(TraceRec), MALLOC_CAP_SPIRAM);
  if (!g_tr) {
    Serial.println("[s3] MSC_TRACE: PSRAM alloc failed, trace off");
    return;
  }
  g_tr_boot = (TraceRec *)heap_caps_malloc(BOOT_KEEP * sizeof(TraceRec), MALLOC_CAP_SPIRAM);
  trace_event(EV_BOOT, TRACE_CAP, 0, 0);
  xTaskCreatePinnedToCore(trace_task, "msc_trace", 6144, nullptr, 1, nullptr, 0);
}

#endif  // MSC_TRACE && FAT_DISK_SHARED_H
