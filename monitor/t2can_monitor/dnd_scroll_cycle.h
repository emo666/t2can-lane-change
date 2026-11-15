#pragma once
#include <stdint.h>

namespace t2can {

// Single-threaded, synchronous API. All timestamps are monotonic uint32_t
// milliseconds; call service and observeFrame at least once per 2^31 ms.
// The caller must report physical frames only (not our transmitted frames).
// The sender must synchronously copy the 8 bytes. Submitted means accepted,
// not delivered; Busy means definitely not submitted; Indeterminate means it
// may have been submitted and must never be retried. Stopping or invalidating
// does not transmit a neutral frame; integration must account for that.
class DndScrollCycle {
 public:
  enum class SendResult : uint8_t { Submitted, Busy, Indeterminate };
  typedef SendResult (*Sender)(void* context, const uint8_t data[8]);
  static constexpr uint32_t kTemplateMaxAgeMs = 250u;
  static constexpr uint32_t kManualQuietMs = 300u;
  static constexpr uint32_t kStepMs = 100u;
  static constexpr uint32_t kRepeatMs = 2000u;
  static constexpr uint32_t kRetryMs = 100u;
  static constexpr uint8_t kMaxAttempts = 3u; // initial attempt included
  static constexpr uint32_t kMaxLatenessMs = 100u;

  // Disabling cancels all work immediately, including retries. Enabling starts
  // a new sequence as soon as the other conditions are satisfied.
  void setEnabled(bool enabled) {
    if (!enabled) { active_ = false; waiting_ = false; cooldownPending_ = false; attempts_ = 0; }
    enabled_ = enabled;
  }

  // Call after a receive backlog or other loss of freshness guarantees. Keep
  // the enabled setting and any existing inter-group cooldown unchanged.
  // Without a timestamp, a cancelled group's cooldown starts at the next
  // timestamped API call. Prefer invalidateTemplateAt when time is known.
  void invalidateTemplate() {
    templateValid_ = false;
    if (active_) cooldownPending_ = true;
    active_ = false;
    attempts_ = 0;
  }

  void invalidateTemplateAt(uint32_t now) {
    settleCooldown(now);
    templateValid_ = false;
    cancelActive(now);
  }

  // After an untrusted backlog, require a fresh template and a quiet period.
  void requireQuietAfterBacklog(uint32_t now) {
    invalidateTemplateAt(now);
    manualSeen_ = true;
    lastManual_ = now;
  }

  // Only standard 0x3C2 mux1 frames are relevant. Invalid DLC invalidates
  // the old template; unrelated IDs, extended frames and other muxes are ignored.
  // Every non-neutral physical mux1 frame both invalidates the template and
  // restarts the manual quiet period. A new neutral frame is then required.
  void observeFrame(uint32_t now, uint32_t id, bool extended, uint8_t dlc,
                    const uint8_t* data) {
    settleCooldown(now);
    if (extended || id != 0x3c2u) return;
    if (!data || dlc != 8u) {
      templateValid_ = false;
      cancelActive(now);
      return;
    }
    if ((data[0] & 3u) != 1u) return;
    if (templateValid_ && uint32_t(now - templateTime_) > kTemplateMaxAgeMs) {
      cancelActive(now);
    }
    if ((data[2] & 0x3fu) != 0u || (data[3] & 0x3fu) != 0u) {
      manualSeen_ = true;
      lastManual_ = now;
      templateValid_ = false;
      cancelActive(now);
      return;
    }
    for (uint8_t i = 0; i < 8; ++i) template_[i] = data[i];
    templateTime_ = now;
    templateValid_ = true;
  }

  // At most one sender invocation per call. A missing sender is treated as
  // ineligible, never as a successful transmission. Call frequently enough
  // for 100 ms cadence; a step over 100 ms late is discarded with its group.
  void service(uint32_t now, Sender sender, void* context) {
    settleCooldown(now);
    if (!enabled_) return;
    const bool eligible = sender && templateValid_ &&
        uint32_t(now - templateTime_) <= kTemplateMaxAgeMs &&
        (!manualSeen_ || uint32_t(now - lastManual_) >= kManualQuietMs);
    if (!eligible) { cancelActive(now); return; }
    if (waiting_) {
      if (uint32_t(now - waitStart_) < kRepeatMs) return;
      waiting_ = false;
    }
    if (!active_) {
      active_ = true;
      step_ = 0;
      attempts_ = 0;
      dueStart_ = now;
      dueDelay_ = 0;
    }
    const uint32_t elapsed = uint32_t(now - dueStart_);
    if (elapsed < dueDelay_) return;
    if (elapsed - dueDelay_ > kMaxLatenessMs) {
      cancelActive(now);
      return;
    }
    uint8_t output[8];
    for (uint8_t i = 0; i < 8; ++i) output[i] = template_[i];
    const uint8_t ticks[4] = {1u, 0u, 0x3fu, 0u};
    output[3] = uint8_t((output[3] & 0xc0u) | ticks[step_]);
    if (ticks[step_] == 0u) output[6] |= 0x10u;
    else output[6] &= 0xefu;
    const SendResult result = sender(context, output);
    if (result != SendResult::Submitted) {
      if (result != SendResult::Busy || ++attempts_ >= kMaxAttempts) {
        active_ = false;
        attempts_ = 0;
        waiting_ = true;
        waitStart_ = now;
      } else {
        dueStart_ = now;
        dueDelay_ = kRetryMs;
      }
      return;
    }
    attempts_ = 0;
    if (++step_ == 4u) {
      active_ = false;
      waiting_ = true;
      waitStart_ = now;
    } else {
      dueStart_ = now;
      dueDelay_ = kStepMs;
    }
  }

 private:
  void settleCooldown(uint32_t now) {
    if (cooldownPending_) {
      cooldownPending_ = false;
      waiting_ = true;
      waitStart_ = now;
    }
  }
  void cancelActive(uint32_t now) {
    if (active_) {
      active_ = false;
      waiting_ = true;
      waitStart_ = now;
    }
    attempts_ = 0;
  }
  bool cooldownPending_ = false;
  bool enabled_ = false, templateValid_ = false, manualSeen_ = false;
  bool active_ = false, waiting_ = false;
  uint8_t template_[8] = {}, step_ = 0, attempts_ = 0;
  uint32_t templateTime_ = 0, lastManual_ = 0, dueStart_ = 0;
  uint32_t dueDelay_ = 0, waitStart_ = 0;
};
} // namespace t2can
