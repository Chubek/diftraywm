# LibShell

LibShell is a header-only C++20 shell runtime and DSL for typed command execution, validated process graphs, POSIX-style redirection, pipelines, shell state, programmable stream sinks, kernels, and CLI integration.

## Manual

- [01. Architecture](@ref manual_architecture)
- [02. Installation](@ref manual_installation)
- [03. Build Integration](@ref manual_build_integration)
- [04. Typed Commands](@ref manual_typed_commands)
- [05. Arguments and Expansion](@ref manual_arguments_expansion)
- [06. Environment](@ref manual_environment)
- [07. Redirection and Stdio](@ref manual_redirection_stdio)
- [08. Pipelines](@ref manual_pipelines)
- [09. Sequencing and Subshells](@ref manual_sequencing_subshells)
- [10. Execution Reports](@ref manual_execution_reports)
- [11. POSIX Executor](@ref manual_posix_executor)
- [12. Dry Run and Validation](@ref manual_dry_run_validation)
- [13. Sinklets](@ref manual_sinklets)
- [14. Kernels](@ref manual_kernels)
- [15. Standard Kernels](@ref manual_standard_kernels)
- [16. Parser and CLI](@ref manual_parser_cli)
- [17. Configuration and Policy](@ref manual_configuration_policy)
- [18. Testing and Debugging](@ref manual_testing_debugging)
- [19. Packaging and Installation](@ref manual_packaging_installation)
- [20. Extension Roadmap](@ref manual_extension_roadmap)

## Reference Inputs

- Public API: `include/LibShell.hpp`
- POSIX executor: `include/LibShell-Posix.hpp`
- Kernel ABI: `include/LibShell-Kernel.hpp`
- Standard kernels: `stdkern/StdKern.hpp`
- CLI frontend: `cli/libsh-cli.cpp`, `cli/libsh-parse.cpp`, `cli/Shell.syntax`
