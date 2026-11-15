#pragma once
#include <stdint.h>

// Standalone P4 ordinary TLSSC policy. Caller serializes all calls and reports
// each proposed transmission exactly once before preparing the next frame.
namespace tlssc {
struct Config {
  bool enabled = false;
  bool block_highway = false;
  bool block_noa = false;
};
struct State {
  bool ap_valid = false;
  uint8_t ap_state = 0xff;
  bool road_valid = false;
  bool gps_match = false;
  uint32_t road_ms = 0;
  uint8_t positive = 0;
  uint8_t negative = 0;
  bool highway = false;
  bool injected = false;
  bool clear_pending = false;
};
struct Road {
  bool valid = false;
  uint8_t road_class = 0;
  bool gps_match = false;
  bool nav_route = false;
  bool controlled_access = false;
  bool left_off_ramp = false;
  bool right_off_ramp = false;
};
inline bool observe_ap(State& s, const uint8_t* data, uint8_t dlc) {
  if (!data || dlc < 6) return false;
  s.ap_valid = true;
  s.ap_state = data[0] & 0x0f;
  return true;
}
inline bool ap_active(const State& s) {
  return s.ap_valid && s.ap_state >= 3 && s.ap_state <= 6;
}
inline bool noa_active(const State& s) { return s.ap_valid && s.ap_state == 5; }
inline Road decode_road(const uint8_t* data, uint8_t dlc) {
  Road r;
  if (!data || dlc < 5) return r;
  r.valid = true;
  r.road_class = (data[0] >> 3) & 7;
  r.gps_match = (data[3] & 0x10) != 0;
  r.nav_route = (data[3] & 0x20) != 0;
  r.controlled_access = (data[4] & 0x20) != 0;
  r.left_off_ramp = (data[4] & 0x40) != 0;
  r.right_off_ramp = (data[4] & 0x80) != 0;
  return r;
}
inline bool observe_road(State& s, const uint8_t* data, uint8_t dlc, uint32_t now) {
  const Road r = decode_road(data, dlc);
  if (!r.valid) return false;
  s.road_valid = true; s.gps_match = r.gps_match; s.road_ms = now;
  if (!r.gps_match) { s.positive = s.negative = 0; s.highway = false; }
  else if (r.controlled_access) {
    s.negative = 0;
    if (s.positive < 2) ++s.positive;
    if (s.positive >= 2) s.highway = true;
  } else {
    s.positive = 0;
    if (s.negative < 2) ++s.negative;
    if (s.negative >= 2) s.highway = false;
  }
  return true;
}
inline bool highway_blocked(const Config& c, const State& s, uint32_t now) {
  return c.block_highway && s.road_valid && s.gps_match && s.road_ms != 0 &&
         uint32_t(now - s.road_ms) <= 2000u && s.highway;
}
enum class Action { none, enable, clear };
struct Decision { Action action = Action::none; uint8_t data[8] = {}; };
// Only standard data 0x3FD mux0 with an eight-byte stock payload is accepted.
inline Decision prepare(State& s, const Config& c, uint32_t now, uint32_t id,
                        bool extended, bool remote, const uint8_t* data, uint8_t dlc) {
  Decision d;
  if (id != 0x3fd || extended || remote || !data || dlc != 8 || (data[0] & 7)) return d;
  for (unsigned i = 0; i < 8; ++i) d.data[i] = data[i];
  const bool desired = c.enabled && ap_active(s) && !highway_blocked(c, s, now) &&
                       !(c.block_noa && noa_active(s));
  if (!desired) {
    if (s.injected) s.clear_pending = true;
    if (s.clear_pending) { d.data[4] &= 0x3fu; d.action = Action::clear; }
  } else {
    s.clear_pending = false;
    if ((d.data[4] & 0xc0u) != 0xc0u) { d.data[4] |= 0xc0u; d.action = Action::enable; }
  }
  return d;
}
// A rejected admission has the same retry state as a failed send. Never report
// success unless the transport actually accepted the prepared frame.
inline void complete(State& s, Action action, bool success) {
  if (action == Action::clear) {
    if (success) { s.injected = false; s.clear_pending = false; }
    else s.clear_pending = true;
  } else if (action == Action::enable && success) s.injected = true;
}
}  // namespace tlssc
