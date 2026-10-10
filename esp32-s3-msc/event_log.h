// LINK_EVENTS: the S3 keeps the last EVENT_LOG_CAP unusual events in flash,
// so a drive can be checked afterwards without a laptop in the car (Muni,
// 2026-10-10: "just save events, not full trash"). Only things worth knowing:
// link damage bursts, the link going down, the classic restarting, silence
// served to the radio, catch-up jumps, USB re-attaches, encoder failure, an
// abnormal S3 reset. Each event is written once, when it has ended.
//
// 16 bytes each, in a ring: the oldest drops out. Storage is a template
// parameter so the host test can use RAM. Store must provide:
//   bool get(uint32_t slot, LinkEvent &e); void put(uint32_t slot, const LinkEvent &e);
//   uint32_t get_count(); void put_count(uint32_t n);
#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct LinkEvent {
  uint16_t boot;   // S3 boot number (one per drive)
  uint8_t type;    // EV_*
  uint8_t flags;
  uint32_t t_ms;   // ms since that boot when the event started
  uint32_t a, b;   // per type, see event_describe()
};
static_assert(sizeof(LinkEvent) == 16, "LinkEvent must stay 16 bytes (stored format)");

enum : uint8_t {
  EV_LOSS = 1,           // a = duration ms, b = lost frames (low 16) | repaired frames (high 16); flags = outages
  EV_LINK_DOWN = 2,      // a = duration ms
  EV_CLASSIC_RESET = 3,  // a = first 4 chars of the classic's reset reason
  EV_UNDERRUN = 4,       // a = duration ms, b = silent reads served
  EV_JUMP = 5,           // a = catch-up jumps in the episode
  EV_USB_REATTACH = 6,   // a = attempts so far
  EV_ENCODER_FAIL = 7,
  EV_S3_RESET = 8,       // a = esp_reset_reason()
};

static const uint32_t EVENT_LOG_CAP = 64;

template <class Store>
class EventLog {
 public:
  explicit EventLog(Store &s) : s_(s) {}
  uint32_t count() { return s_.get_count(); }  // ever written (the ring keeps the last CAP)
  void add(const LinkEvent &e) {
    uint32_t n = s_.get_count();
    s_.put(n % EVENT_LOG_CAP, e);
    s_.put_count(n + 1);
  }
  // Oldest first.
  template <class F>
  void for_each(F f) {
    uint32_t n = s_.get_count(), first = n > EVENT_LOG_CAP ? n - EVENT_LOG_CAP : 0;
    for (uint32_t i = first; i < n; i++) {
      LinkEvent e;
      if (s_.get(i % EVENT_LOG_CAP, e)) f(i, e);
    }
  }
  void clear() { s_.put_count(0); }

 private:
  Store &s_;
};

// One line per event.
static inline void event_describe(const LinkEvent &e, char *buf, size_t n) {
  uint32_t s = e.t_ms / 1000;
  char when[32];
  snprintf(when, sizeof when, "boot %u +%lu:%02lu:%02lu", (unsigned)e.boot, (unsigned long)(s / 3600),
           (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  switch (e.type) {
    case EV_LOSS:
      snprintf(buf, n, "%s  link damage %lu ms: %lu frames filled, %lu repaired%s", when, (unsigned long)e.a,
               (unsigned long)(e.b & 0xFFFF), (unsigned long)(e.b >> 16), e.flags ? ", with a gap" : "");
      break;
    case EV_LINK_DOWN:
      snprintf(buf, n, "%s  link down %lu ms (no data from the classic)", when, (unsigned long)e.a);
      break;
    case EV_CLASSIC_RESET: {
      char r[5];
      memcpy(r, &e.a, 4);
      r[4] = 0;
      snprintf(buf, n, "%s  classic restarted, reason %s", when, r);
      break;
    }
    case EV_UNDERRUN:
      snprintf(buf, n, "%s  silence served %lu ms (%lu reads)", when, (unsigned long)e.a, (unsigned long)e.b);
      break;
    case EV_JUMP:
      snprintf(buf, n, "%s  catch-up jump x%lu", when, (unsigned long)e.a);
      break;
    case EV_USB_REATTACH:
      snprintf(buf, n, "%s  USB re-attach (radio hadn't picked up the drive), attempt %lu", when, (unsigned long)e.a);
      break;
    case EV_ENCODER_FAIL:
      snprintf(buf, n, "%s  MP3 encoder failed to start", when);
      break;
    case EV_S3_RESET:
      snprintf(buf, n, "%s  S3 started after an abnormal reset (reason %lu)", when, (unsigned long)e.a);
      break;
    default:
      snprintf(buf, n, "%s  unknown event %u", when, (unsigned)e.type);
  }
}

// Opens an episode on activity, closes it after `quiet_ms` without any.
// update() returns true when an episode just closed (read start/duration/sum).
struct Episode {
  bool open = false;
  uint32_t start = 0, last = 0, sum = 0, sum2 = 0, extra = 0;
  bool update(uint32_t now, uint32_t delta, uint32_t delta2, uint32_t quiet_ms) {
    if (delta || delta2) {
      if (!open) { open = true; start = now; sum = sum2 = extra = 0; }
      last = now;
      sum += delta;
      sum2 += delta2;
      return false;
    }
    if (open && now - last >= quiet_ms) { open = false; return true; }
    return false;
  }
  uint32_t duration() const { return last - start; }
};

#endif  // EVENT_LOG_H
