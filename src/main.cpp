#include "compositor/Compositor.hpp"

int main() {
  Compositor compositor;
  if (!compositor.init()) {
    return 1;
  }
  return compositor.run();
}
