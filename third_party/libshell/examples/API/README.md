# LibShell API Examples

Build:

```sh
cmake -S examples/API -B examples/API/build
cmake --build examples/API/build -j
```

Examples:

- `01_dry_run_plan.cpp` — inspect expanded `ExecSpec` without spawning;
- `02_echo_capture.cpp` — capture builtin stdout in memory;
- `03_variable_expansion.cpp` — expand shell variables;
- `04_environment_overlay.cpp` — pass command-local environment;
- `05_export_and_child_env.cpp` — export a variable for child processes;
- `06_command_working_directory.cpp` — run one command in a specific directory;
- `07_stdout_to_file.cpp` — redirect stdout to a file;
- `08_append_to_file.cpp` — append multiple writes to one file;
- `09_stderr_capture.cpp` — capture stderr separately;
- `10_discard_stderr.cpp` — suppress noisy stderr;
- `11_pipeline_count_lines.cpp` — build a counting pipeline;
- `12_pipefail_any_failed.cpp` — propagate pipeline failure policy;
- `13_and_if_short_circuit.cpp` — gate work behind `&&`;
- `14_or_if_recovery.cpp` — recover with `||`;
- `15_subshell_copy_isolation.cpp` — isolate transient state;
- `16_subshell_shared_environment.cpp` — persist shared subshell mutation;
- `17_arithmetic_expansion.cpp` — evaluate arithmetic fragments;
- `18_command_substitution.cpp` — use runtime-backed `$(...)`;
- `19_command_substitution_parser.cpp` — enable full parser-backed `$(...)`;
- `20_glob_expansion.cpp` — expand file globs;
- `21_validation_diagnostics.cpp` — inspect IR validation failures;
- `22_lambda_sinklet.cpp` — stream output through a lambda sinklet;
- `23_grep_sinklet.cpp` — filter lines with `GrepSinklet`;
- `24_json_lines_sinklet.cpp` — convert lines to JSONL;
- `25_function_kernel.cpp` — register and invoke a runtime kernel.
