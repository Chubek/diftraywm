#include "nterm/NTerm.hpp"

class GlyphRenderer;

class NTermRenderer {
public:
  NTermRenderer(NTerm *term, GlyphRenderer *renderer) : term_(term), renderer_(renderer) {}
  void render(void *, int, int, int, int) {}

private:
  NTerm *term_ = nullptr;
  GlyphRenderer *renderer_ = nullptr;
};
