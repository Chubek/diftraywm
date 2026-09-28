# 17. Configuration and Policy {#manual_configuration_policy}

## ShellOptions

`ShellOptions` centralizes runtime policy. It controls execution defaults without changing IR semantics.

## Policy Domains

- default pipefail behavior;
- timeout/resource defaults;
- environment inheritance defaults;
- executor selection;
- extension registry availability;
- dry-run versus local execution.

## Constraints

- Policy modifies dispatch, not command meaning.
- Security policy must be explicit and inspectable.
- Extension loading must not happen as a side effect of parsing.
- Host environment import must be opt-in at shell construction/configuration time.

## Recommended Shape

Keep policy declarative. Prefer immutable option snapshots passed into `Shell` over ambient globals.
