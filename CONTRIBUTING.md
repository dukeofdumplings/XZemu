# Contributing to XZemu

Please test an initial change on the platform you use before proposing it.
This project is deliberately close to the original hardware and software, so
an apparently small host-side or device-model change can affect boot, timing,
or an original Pioneer process in unexpected ways.

For a non-Apple host platform, keep its launcher, audio, controls, and build
logic appropriately separate from the macOS implementation. Do not turn a
platform-specific workaround into a requirement for every build.

If there is a direction you strongly want to pursue that does not fit the
current project, forks are welcome. The source is here to be useful for that
kind of experimentation too.
