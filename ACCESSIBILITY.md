# Accessibility statement

Refrain is a headless server. It ships no graphical client and no in-game user
interface, so the surfaces a person interacts with are the repository
documentation, the `refrain-server` console and the `refrain-tools` command
line. This statement describes those surfaces as they are today. It records
what has been checked and what has not; it is not a conformance claim.

Accessibility of the Minecraft client itself is outside this project. Refrain
only speaks the protocol; the client's own settings, controls and assistive
features are Mojang's, and problems with them belong in the client's support
channels.

## What the project commits to

- **Plain Markdown documentation.** Every guide is Markdown with a real heading
  hierarchy: one title per file, sections nested in order, no heading level
  skipped for visual effect. The rendered page therefore has the outline a
  screen reader can navigate by heading.
- **Self-describing links.** Link text names its destination; the documentation
  does not use "click here" or bare URLs in prose. File names, commands and
  configuration keys are written in backticks.
- **No colour-only or image-only information.** The documentation contains no
  diagrams and no screenshots, and nothing is conveyed by colour alone. Tables
  are used for tabular data and always carry a header row.
- **Plain-text tools.** `refrain-server` and `refrain-tools` write text.
  Failures are reported as a text message on standard error (operational errors
  carry an error code) and a failing command returns a non-zero exit status, so
  a pipe, a log file or a screen reader gets the same information as a
  terminal.
- **Separated streams.** Machine-readable output goes to standard output and
  diagnostics go to standard error, so redirecting one never mixes the other.
  Command output can be captured with `>out.txt` while messages remain on the
  terminal or in `2>err.txt`.
- **Decorative colour only.** Console colour marks a log line's level, and the
  level word (`info`, `warn`, `error`) is part of the line, so the colour never
  carries information by itself. Colour is emitted only when standard error is
  an interactive terminal that advertises colour support through `TERM` or
  `COLORTERM` (the bundled spdlog sink detects this automatically). Redirected
  output, a pipe, a log file and a non-colour terminal receive plain text. There
  is currently no `--no-color` option or colour key in the configuration, and
  the conventional `NO_COLOR` variable is not read; redirecting standard error
  is the reliable way to get plain text from a colour terminal.
- **Machine-readable output.** Several commands offer a text form and a
  structured form: `refrain-tools world info --json` prints JSON,
  `--dump-gamepack` prints the resolved composition, and log lines keep a stable
  `[time] [thread/level] [module]: message` prefix so they can be parsed.
- **Bounded output when you want it.** `refrain-server --check` validates the
  configuration, the mod composition and the lock and exits without creating a
  world. `--ticks <n>` bounds a run, and `--hash-every 0` keeps the state hash
  out of the log (the default).

## Supported environments

| Environment | Support |
|---|---|
| Linux x86_64 | Primary target. Built and tested locally and in CI. |
| Windows x86_64 | MinGW and MSVC builds are compiled and tested in CI and ship as the Windows archives; not the primary development environment. |
| macOS | Not supported. |
| Display, GPU, audio | Not required. The server is headless and runs over SSH or a plain console. A display is needed only for the optional developer scripts that drive the unmodified client under XWayland. |
| Terminal | Any terminal that renders plain text; colour output additionally needs a terminal that advertises colour support. |

## Known limitations

- **No graphical client or GUI.** Every operation is a command line or a typed
  console command. There is no pointer-driven or menu-driven alternative for
  starting a server, editing configuration or managing a world.
- **No formal accessibility testing yet.** The documentation and tool output
  have not been tested with screen readers, keyboard-only navigation, browser
  zoom or a contrast checker, and no WCAG conformance level is claimed. The
  statements above describe properties of the documents and the output, not
  results of an audit.
- **English only.** The documentation and the messages emitted by the server and
  tools are written in English and are not translated.
- **Long terminal output.** A busy server prints one line per lifecycle and
  network event, so a long session produces a lot of text and the console
  offers no paging or search. Lower the level with
  `refrain-server --log-level warn` (or `error`, `critical`, `off`; the default
  is `info`), or set `[logging] level` in `<config-dir>/refrain.toml`; the
  command line wins over the file. `refrain-tools` has `-v`/`--verbose`, which
  raises its console level to `debug`; the default is `info`. Redirecting
  standard error to a file keeps the terminal quiet.
- **Text-only errors.** Failures are reported as an error message plus an error
  code, without a machine-navigable identifier or a documentation anchor per
  code.
- **Optional diagnostics are binary.** Crash reporting is opt-in with
  `--crash-dir <dir>` and writes local minidumps, which are not readable as
  text. Nothing is uploaded.

## Reporting an accessibility barrier

Open an issue on the repository with `accessibility` in the title and pick the
form that fits closest (the issue templates are the only entry point; blank
issues are disabled). Say what you were doing, the exact command or page, and
what got in the way — for example the terminal, the screen reader, or the part
of the output that was hard to follow. The project keeps an `accessibility`
label for triage.

If a report would contain personal or otherwise sensitive information, use the
private channel described in [SECURITY.md](SECURITY.md) instead of a public
issue.

Documentation fixes are the fastest kind to make; a report that names the page
and the sentence is actionable immediately. There is no formal response time
for accessibility reports, but they are read by a maintainer.
