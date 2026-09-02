#pragma once

#include <vector>

struct Animation {
  double progress = 0.0;
};

class AnimationPipeline {
public:
  void tick(double delta) {
    (void)delta;
  }

private:
  std::vector<Animation> queue_;
};
