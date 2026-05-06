# Contributing

This project is intentionally small: C11, POSIX/Linux system calls, no runtime
dependencies beyond the platform libraries used by the Makefile.

Before opening a pull request:

- Run `make clean && make test`.
- Keep changes focused and match the existing C style.
- Avoid new dependencies unless the tradeoff is explicit and documented.
- Update README, CHANGELOG, or ROADMAP when behavior or user-facing workflow
  changes.

For vulnerability reports, follow `SECURITY.md` instead of opening a public
issue with exploit details.
