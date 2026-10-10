// Host test for esp32-s3-msc/event_log.h (LINK_EVENTS).
//   g++ -O2 -std=c++17 -Wall -o event_log_test event_log_test.cpp && ./event_log_test
#include <cstdio>
#include <map>
#include <string>
#include <cstring>
#include "../event_log.h"

struct RamStore {
  std::map<uint32_t, LinkEvent> slots;
  uint32_t n = 0;
  bool get(uint32_t s, LinkEvent &e) { auto it = slots.find(s); if (it == slots.end()) return false; e = it->second; return true; }
  void put(uint32_t s, const LinkEvent &e) { slots[s] = e; }
  uint32_t get_count() { return n; }
  void put_count(uint32_t c) { n = c; }
};

static int fails = 0;
static void check(bool ok, const char *what) { printf("  %-60s %s\n", what, ok ? "OK" : "FAIL"); fails += !ok; }

int main() {
  RamStore st;
  EventLog<RamStore> log(st);
  for (uint32_t i = 0; i < 100; i++) log.add({1, EV_LOSS, 0, i * 1000, 500, 3});
  uint32_t seen = 0, first = 0, last = 0;
  bool ordered = true;
  log.for_each([&](uint32_t idx, const LinkEvent &e) {
    if (seen == 0) first = e.t_ms;
    if (seen && e.t_ms != last + 1000) ordered = false;
    last = e.t_ms;
    seen++;
    (void)idx;
  });
  check(log.count() == 100 && seen == EVENT_LOG_CAP, "100 written: the ring keeps the last 64");
  check(first == 36000 && last == 99000 && ordered, "oldest first, newest last, in order");
  log.clear();
  seen = 0;
  log.for_each([&](uint32_t, const LinkEvent &) { seen++; });
  check(seen == 0, "clear empties it");

  char line[120];
  event_describe({3, EV_LOSS, 1, 3723000, 2000, 12 | (5u << 16)}, line, sizeof line);
  printf("    %s\n", line);
  check(std::string(line).find("boot 3 +1:02:03") != std::string::npos && std::string(line).find("12 frames filled, 5 repaired") != std::string::npos,
        "a loss event reads 'boot 3 +1:02:03 ... 12 frames filled, 5 repaired'");
  uint32_t r = 0;
  memcpy(&r, "BROW", 4);
  event_describe({3, EV_CLASSIC_RESET, 0, 5000, r, 0}, line, sizeof line);
  printf("    %s\n", line);
  check(std::string(line).find("reason BROW") != std::string::npos, "a classic reset shows its reason");

  // Episodes: activity at 10, 11, 12 s, then quiet; closes 2 s after the last.
  Episode ep;
  bool closed = false;
  uint32_t at = 0;
  for (uint32_t t = 9000; t <= 20000; t += 1000) {
    uint32_t d = (t >= 10000 && t <= 12000) ? 4 : 0;
    if (ep.update(t, d, t == 11000 ? 2 : 0, 2000)) { closed = true; at = t; break; }
  }
  check(closed && at == 14000 && ep.start == 10000 && ep.duration() == 2000 && ep.sum == 12 && ep.sum2 == 2,
        "episode: opens on activity, closes after 2 quiet seconds, sums both counts");

  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails ? 1 : 0;
}
