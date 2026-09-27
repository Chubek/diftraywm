# Extending AurocksGLR

AurocksGLR has two stages:

1. `AurocksGLR.pl` reads a grammar and writes an intermediate `.m4` stream.
2. GNU `m4` evaluates that stream using a target macro set.

The generated stream calls these macros (all arguments use `<<<...>>>` quoting):

| Macro | Arguments | Meaning |
| --- | --- | --- |
| `AUROCKS_START` | entrypoint | Start production/API name |
| `AUROCKS_PROLOGUE` | text | Grammar prologue |
| `AUROCKS_SKIP` | regexp | Layout expression |
| `AUROCKS_DIRECTIVE` | name, value | Any grammar directive |
| `AUROCKS_RULE` | lhs, rhs, action, dprec | One production alternative |
| `AUROCKS_EPILOGUE` | text | Grammar epilogue |
| `AUROCKS_C_SOURCE` | text | Reference C rendering |

Target files should include `support.m4`, which defines every macro as a
no-op. Override the macros needed by your backend. `AUROCKS_C_SOURCE` is a
convenient compatibility hook: the bundled C target emits it verbatim, while
other targets can replace it with a complete backend implementation.

Use a custom target with:

```sh
AurocksGLR.sh --targets-dir ./my-targets -T MyTarget grammar.g
```

The `.m4` suffix is optional. Multiple directories can be supplied through
the comma-separated `AUROCKSGLR_TARGET_PATH` environment variable. A custom
macro file should not assume the current working directory; `include` files
are searched relative to the macro file directory by the driver.
