#pragma once

#include "theme/ThemeEngine.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <utility>

// Timelines are driven by the compositor loop. Callbacks only update scene
// opacity; no terminal rasterisation or allocation is needed for each frame.
class AnimationPipeline {
public:
  using Setter = std::function<void(double)>;

  void start(const void *owner, const AnimationSpec &spec, Setter setter) {
    cancel(owner);
    if (spec.duration <= 0 || spec.frames.empty()) { setter(1); return; }
    Timeline timeline{spec, std::move(setter), 0};
    timeline.setter(spec.delay <= 0 || spec.backwards ? spec.frames.front().opacity : 1);
    queue_.emplace(owner, std::move(timeline));
  }
  void cancel(const void *owner) {
    auto found = queue_.find(owner);
    if (found != queue_.end()) { found->second.setter(1); queue_.erase(found); }
  }
  void clear() {
    for (auto &[owner, timeline] : queue_) timeline.setter(1);
    queue_.clear();
  }
  bool active() const { return !queue_.empty(); }
  void tick(double delta) {
    if (!std::isfinite(delta) || delta < 0) return;
    for (auto it = queue_.begin(); it != queue_.end();) {
      auto &timeline = it->second;
      timeline.elapsed += delta;
      const auto &spec = timeline.spec;
      const double time = timeline.elapsed - spec.delay;
      if (time < 0) { ++it; continue; }
      const double progress = std::min(1.0, time / spec.duration);
      if (progress >= 1) {
        timeline.setter(spec.forwards ? spec.frames.back().opacity : 1);
        it = queue_.erase(it);
        continue;
      }
      auto upper = std::upper_bound(spec.frames.begin(), spec.frames.end(), progress,
          [](double offset, const AnimationKeyframe &frame) { return offset < frame.offset; });
      const auto &right = upper == spec.frames.end() ? spec.frames.back() : *upper;
      const auto &left = upper == spec.frames.begin() ? spec.frames.front() : *(upper - 1);
      const double span = right.offset - left.offset;
      const double local = span > 0 ? (progress - left.offset) / span : 1;
      timeline.setter(left.opacity + (right.opacity - left.opacity) * ease(spec, local));
      ++it;
    }
  }

private:
  struct Timeline { AnimationSpec spec; Setter setter; double elapsed; };
  static double ease(const AnimationSpec &spec, double progress) {
    progress = std::clamp(progress, 0.0, 1.0);
    if (spec.linear) return progress;
    auto bezier = [](double t, double a, double b) {
      const double rest = 1 - t;
      return 3 * rest * rest * t * a + 3 * rest * t * t * b + t * t * t;
    };
    double low = 0, high = 1;
    for (int i = 0; i < 24; ++i) {
      const double t = (low + high) / 2;
      if (bezier(t, spec.curve[0], spec.curve[2]) < progress) low = t;
      else high = t;
    }
    return bezier((low + high) / 2, spec.curve[1], spec.curve[3]);
  }
  std::unordered_map<const void *, Timeline> queue_;
};
