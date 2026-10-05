#include "theme/AnimationPipeline.hpp"
#include "theme/ThemeEngine.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
}
int main() {
  try {
    ThemeEngine engine;
    ThemeProperties properties;
    const char *css = R"CSS(
      :root { animation: appear 200ms linear; }
      @keyframes appear {
        from { opacity: 0; }
        50% { opacity: 0.8; }
        to { opacity: 1; }
      }
    )CSS";
    check(engine.parse_css(css, properties), "CSS keyframes did not parse");
    double opacity = -1;
    int owner = 0;
    AnimationPipeline pipeline;
    pipeline.start(&owner, properties.cell_animation, [&](double value) { opacity = value; });
    check(opacity == 0 && pipeline.active(), "animation did not start at first keyframe");
    pipeline.tick(0.05);
    check(std::abs(opacity - 0.4) < 0.001, "animation did not interpolate between keyframes");
    pipeline.tick(0.05);
    check(std::abs(opacity - 0.8) < 0.001, "midpoint keyframe was lost");
    pipeline.tick(0.1);
    check(opacity == 1 && !pipeline.active(), "animation did not finish or release its owner");
    pipeline.start(&owner, properties.cell_animation, [&](double value) { opacity = value; });
    pipeline.tick(-1);
    pipeline.tick(std::nan(""));
    check(opacity == 0, "invalid clock delta changed animation");
    pipeline.cancel(&owner);
    check(opacity == 1 && !pipeline.active(), "cancellation did not restore opacity");
    check(engine.parse_css(R"CSS(
      @keyframes fade { from { opacity: 0; } to { opacity: 0.5; } }
      :root { animation-name: fade; animation-duration: 1s; animation-delay: 100ms;
              animation-timing-function: ease-out; animation-fill-mode: both; }
    )CSS", properties), "animation longhands did not parse");
    pipeline.start(&owner, properties.cell_animation, [&](double value) { opacity = value; });
    pipeline.tick(0.05);
    check(opacity == 0, "animation delay was ignored");
    pipeline.tick(0.55);
    check(opacity > 0.25 && opacity < 0.5, "ease-out did not affect opacity");
    pipeline.tick(1);
    check(opacity == 0.5 && !pipeline.active(), "forwards fill did not preserve final keyframe");
    check(engine.parse_css(":root { animation: none; }", properties), "animation disable failed");
    pipeline.start(&owner, properties.cell_animation, [&](double value) { opacity = value; });
    check(opacity == 1 && !pipeline.active(), "disabled animation retained a timeline");
    for (const auto *invalid : {":root { animation: missing 100ms; }", "@keyframes x { 200% { opacity: 0; } }",
                               "@keyframes x { from { opacity: bad; } }", "@keyframes x { from { opacity: 0; }",
                               ":root { animation-duration: -1s; }", ":root { animation-duration: 100000s; }"})
      check(!engine.parse_css(invalid, properties), "invalid animation was accepted");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
