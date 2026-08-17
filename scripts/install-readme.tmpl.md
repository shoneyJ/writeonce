# writeonce @VER@ (@TRIPLE@)

A single-binary compiled language with an embedded, WAL-durable database. This
archive is the toolchain: `woc` (the compiler) and `wovm` (the runtime VM).
Both are native binaries that depend only on the system C library — nothing
else to install.

## Install (Linux)

Remove any previous install and extract this archive into `/usr/local`,
creating a fresh `/usr/local/writeonce`:

    rm -rf /usr/local/writeonce && tar -C /usr/local -xzf writeonce-@VER@-@TRIPLE@.tar.gz

(Run as root, or through `sudo`.)

Add `/usr/local/writeonce/bin` to your `PATH` by adding this line to your
`$HOME/.profile` (or `/etc/profile` for a system-wide install):

    export PATH=$PATH:/usr/local/writeonce/bin

Restart your shell, or `source $HOME/.profile` to apply it now. Then verify:

    woc version        # writeonce @VER@ linux/amd64
    wovm --version     # wovm @VER@

## Use

    woc <project-dir>          # builds <project>/target/<name>, one standalone binary
    ./<project>/target/<name>  # run it — no runtime to install on the target host

A writeonce project is a directory with a `wo.toml` manifest and one or more
`.wo` files. `woc` locates `wovm` beside itself, so a tarball install needs no
extra configuration; override with the `$WO_RUNTIME` environment variable or a
`[build] runtime = "..."` key in `wo.toml` if you ever need to.

A `wo.toml` may declare a minimum toolchain version:

    [runtime]
    wo = ">= @VER@"

`woc` refuses to build a project that requires a newer toolchain than itself.
