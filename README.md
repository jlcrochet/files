# files

Fast recursive file lister. Lists files in a directory tree, printing
relative paths to stdout.

## Build

```
make
make install
```

## Usage

```
files [options] [directory]
```

If no directory is given, the current directory is used.

## Options

| Option | Description |
|---|---|
| `-i`, `--gitignore` | Respect `.gitignore` rules (uses `git ls-files` when inside a repo) |
| `-H`, `--show-hidden` | Show hidden files (dotfiles). By default, hidden files and directories are skipped |
| `-0`, `--null` | Separate output entries with NUL instead of newline |
| `-h`, `--help` | Print help |

## Default behavior

- Hidden files and directories (names starting with `.`) are skipped.
- Output is newline-separated.
- Symbolic links to directories are not followed.

## Dependencies

None. Standard C library only.

When `--gitignore` is used, `git` must be available on `PATH`.
