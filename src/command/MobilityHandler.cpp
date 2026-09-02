#include <algorithm>

int diftraywm_clamp_move_delta(int delta) { return std::clamp(delta, -1, 1); }
