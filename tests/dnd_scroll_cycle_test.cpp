#include "../src/dnd_scroll_cycle.h"

using t2can::DndScrollCycle;
static unsigned failures = 0;
#define CHECK(x) do { if (!(x) && !failures) failures = __LINE__; } while (0)
struct Sink { unsigned calls = 0; DndScrollCycle::SendResult result = DndScrollCycle::SendResult::Submitted; uint8_t data[8] = {}; };
static DndScrollCycle::SendResult send(void* p, const uint8_t* d) {
  Sink& s = *static_cast<Sink*>(p); ++s.calls;
  for (unsigned i = 0; i < 8; ++i) s.data[i] = d[i];
  return s.result;
}
static uint8_t frame[8] = {0xadu, 0x42u, 0x80u, 0xc0u, 0x55u, 0xabu, 0xbfu, 0x77u};
static void observe(DndScrollCycle& c, uint32_t t) { c.observeFrame(t, 0x3c2, false, 8, frame); }
static void tick(DndScrollCycle& c, Sink& s, uint32_t t) { c.service(t, send, &s); }
static void encoding() {
  DndScrollCycle c; Sink s; c.setEnabled(true); observe(c, 0); tick(c, s, 0);
  const uint8_t values[] = {1, 0, 63, 0};
  for (unsigned n = 0; n < 4; ++n) {
    if (n) { tick(c, s, n * 100 - 1); CHECK(s.calls == n); observe(c, n * 100); tick(c, s, n * 100); }
    CHECK(s.calls == n + 1); CHECK((s.data[3] & 63) == values[n]);
    CHECK((s.data[3] & 0xc0) == 0xc0);
    CHECK(((s.data[6] & 0x10) != 0) == (values[n] == 0));
    CHECK((s.data[6] & 0xef) == (frame[6] & 0xef));
    for (unsigned i = 0; i < 8; ++i) if (i != 3 && i != 6) CHECK(s.data[i] == frame[i]);
  }
  observe(c, 2299); tick(c, s, 2299); CHECK(s.calls == 4);
  observe(c, 2300); tick(c, s, 2300); CHECK(s.calls == 5); CHECK((s.data[3] & 63) == 1);
}
static void freshness_and_manual() {
  DndScrollCycle c; Sink s; c.setEnabled(true); observe(c, 0); tick(c, s, 251); CHECK(s.calls == 0);
  observe(c, 300); tick(c, s, 300); CHECK(s.calls == 1);
  tick(c, s, 551); CHECK(s.calls == 1); observe(c, 2550); tick(c, s, 2550); CHECK(s.calls == 1);
  observe(c, 2551); tick(c, s, 2551); CHECK(s.calls == 2); CHECK((s.data[3] & 63) == 1);
  uint8_t input[8]; for (unsigned i=0;i<8;++i) input[i]=frame[i]; input[2] |= 1;
  c.observeFrame(2560, 0x3c2, false, 8, input); observe(c, 2859); tick(c, s, 2859); CHECK(s.calls == 2);
  observe(c, 4559); tick(c, s, 4559); CHECK(s.calls == 2);
  observe(c, 4560); tick(c, s, 4560); CHECK(s.calls == 3); CHECK((s.data[3] & 63) == 1);
  input[2] = frame[2]; input[3] |= 0x3f;
  c.observeFrame(4561, 0x3c2, false, 8, input); observe(c, 6561); tick(c, s, 6561);
  CHECK(s.calls == 4); CHECK((s.data[3] & 63) == 1);
  c.setEnabled(false); tick(c, s, 6661); CHECK(s.calls == 4);
  c.setEnabled(true); observe(c, 6661); tick(c, s, 6661); CHECK(s.calls == 5); CHECK((s.data[3] & 63) == 1);
}
static void retry_and_pause() {
  DndScrollCycle c; Sink s; c.setEnabled(true); observe(c, 0); s.result = DndScrollCycle::SendResult::Busy; tick(c, s, 0);
  for (int i = 0; i < 20; ++i) tick(c, s, 0); CHECK(s.calls == 1);
  tick(c, s, 99); CHECK(s.calls == 1); observe(c, 100); tick(c, s, 100); CHECK(s.calls == 2);
  observe(c, 200); tick(c, s, 200); CHECK(s.calls == 3);
  observe(c, 2199); tick(c, s, 2199); CHECK(s.calls == 3);
  observe(c, 2200); s.result = DndScrollCycle::SendResult::Submitted; tick(c, s, 2200); CHECK(s.calls == 4); CHECK((s.data[3] & 63) == 1);
  observe(c, 2300); s.result = DndScrollCycle::SendResult::Busy; tick(c, s, 2300); CHECK((s.data[3] & 63) == 0);
  observe(c, 2400); s.result = DndScrollCycle::SendResult::Submitted; tick(c, s, 2400); CHECK((s.data[3] & 63) == 0);
  observe(c, 2500); tick(c, s, 2500); CHECK((s.data[3] & 63) == 63);
  c.service(2600, nullptr, nullptr); const unsigned calls = s.calls;
  observe(c, 4599); tick(c, s, 4599); CHECK(s.calls == calls);
  observe(c, 4600); tick(c, s, 4600); CHECK(s.calls == calls + 1); CHECK((s.data[3] & 63) == 1);
}
static void invalidate() {
  DndScrollCycle c; Sink s; c.setEnabled(true); observe(c, 0); tick(c, s, 0);
  c.invalidateTemplate(); tick(c, s, 100); CHECK(s.calls == 1);
  observe(c, 2099); tick(c, s, 2099); CHECK(s.calls == 1);
  observe(c, 2100); tick(c, s, 2100); CHECK(s.calls == 2); CHECK((s.data[3] & 63) == 1);
  for (uint32_t t = 2200; t <= 2400; t += 100) { observe(c, t); tick(c, s, t); }
  CHECK(s.calls == 5); c.invalidateTemplate(); observe(c, 2500); tick(c, s, 2500); CHECK(s.calls == 5);
  observe(c, 4399); tick(c, s, 4399); CHECK(s.calls == 5);
  observe(c, 4400); tick(c, s, 4400); CHECK(s.calls == 6);
}
static void backlog_quiet() {
  DndScrollCycle c; Sink s; c.setEnabled(true); observe(c, 0); tick(c, s, 0);
  c.requireQuietAfterBacklog(10);
  tick(c, s, 310); CHECK(s.calls == 1); // Old template was invalidated.
  observe(c, 2009); tick(c, s, 2009); CHECK(s.calls == 1);
  observe(c, 2010); tick(c, s, 2010); CHECK(s.calls == 2);
  CHECK((s.data[3] & 63) == 1); // Interrupted group was discarded.

  DndScrollCycle q; Sink z; q.setEnabled(true); q.requireQuietAfterBacklog(1000);
  observe(q, 1049); tick(q, z, 1299); CHECK(z.calls == 0);
  tick(q, z, 1300); CHECK(z.calls == 0); // Template is 251 ms old.
  observe(q, 1300); tick(q, z, 1300); CHECK(z.calls == 1);

  DndScrollCycle w; Sink v; w.setEnabled(true); const uint32_t t = 0xffffff80u;
  w.requireQuietAfterBacklog(t); observe(w, t + 100u);
  tick(w, v, t + 299u); CHECK(v.calls == 0);
  tick(w, v, t + 300u); CHECK(v.calls == 1);
}
static void indeterminate() {
  DndScrollCycle c; Sink s; c.setEnabled(true); observe(c, 0);
  s.result = DndScrollCycle::SendResult::Indeterminate; tick(c, s, 0);
  observe(c, 100); tick(c, s, 100); CHECK(s.calls == 1);
  observe(c, 1999); tick(c, s, 1999); CHECK(s.calls == 1);
  observe(c, 2000); s.result = DndScrollCycle::SendResult::Submitted;
  tick(c, s, 2000); CHECK(s.calls == 2); CHECK((s.data[3] & 63) == 1);
}
static void frame_validation_and_deadlines() {
  DndScrollCycle c; Sink s; c.setEnabled(true);
  c.observeFrame(0, 0x3c2, true, 8, frame); tick(c, s, 0); CHECK(s.calls == 0);
  c.observeFrame(0, 0x3c3, false, 8, frame); tick(c, s, 0); CHECK(s.calls == 0);
  uint8_t other[8]; for (unsigned i=0;i<8;++i) other[i]=frame[i]; other[0] &= 0xfc;
  c.observeFrame(0, 0x3c2, false, 8, other); tick(c, s, 0); CHECK(s.calls == 0);
  observe(c, 0); tick(c, s, 0); CHECK(s.calls == 1);
  c.observeFrame(10, 0x3c2, false, 7, frame); observe(c, 20); tick(c, s, 100); CHECK(s.calls == 1);
  observe(c, 2010); tick(c, s, 2010); CHECK(s.calls == 2); CHECK((s.data[3] & 63) == 1);
  observe(c, 2110); observe(c, 2211); tick(c, s, 2211); CHECK(s.calls == 2); // missed step discarded
  observe(c, 4210); tick(c, s, 4210); CHECK(s.calls == 2);
  observe(c, 4211); tick(c, s, 4211); CHECK(s.calls == 3); CHECK((s.data[3] & 63) == 1);
  DndScrollCycle boundary; Sink b; boundary.setEnabled(true); observe(boundary, 0); tick(boundary, b, 250); CHECK(b.calls == 1);
}
static void wrapping() {
  DndScrollCycle c; Sink s; c.setEnabled(true); const uint32_t t = 0xffffff00u;
  observe(c, t); tick(c, s, t); observe(c, t+100u); tick(c, s, t+100u);
  observe(c, t+200u); tick(c, s, t+200u); observe(c, t+300u); tick(c, s, t+300u);
  CHECK(s.calls == 4); observe(c, t+2299u); tick(c, s, t+2299u); CHECK(s.calls == 4);
  observe(c, t+2300u); tick(c, s, t+2300u); CHECK(s.calls == 5);
  uint8_t manual[8]; for (unsigned i=0;i<8;++i) manual[i]=frame[i]; manual[3] |= 1;
  DndScrollCycle m; Sink z; m.setEnabled(true); m.observeFrame(t,0x3c2,false,8,manual);
  observe(m,t+299u); tick(m,z,t+299u); CHECK(z.calls == 0);
  tick(m,z,t+300u); CHECK(z.calls == 1);
}
int main() {
  encoding(); freshness_and_manual(); retry_and_pause(); invalidate(); backlog_quiet(); indeterminate(); frame_validation_and_deadlines(); wrapping();
  return static_cast<int>(failures);
}
