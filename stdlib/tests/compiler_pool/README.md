This fixture configures two independent native packages and one consumer using
`add_cangjie_library` from the production module. Each package produces static
and BC outputs; the production dependency and cache ordering rules are unchanged.

Run configuration checks with:

```
python3 test_configuration.py --output /path/to/private/configuration-results
```

To compile, use the matching compiler, backend tools and runtime on PATH and in
CANGJIE_HOME, then configure this directory with Ninja and
`-DCANGJIE_MAX_COMPILER_PROCESSES=2`. Building the default target executes all six
compiler edges. Configuration checks alone do not establish a process bound.
