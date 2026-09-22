# Jailcell

Jailcell is a command-line utility for storing text notes and binary files within an encrypted container. It uses SQLCipher to provide transparent AES-256 encryption for the underlying SQLite database.

<img width="1680" height="1019" alt="Screenshot 2026-09-22 at 15 57 22" src="https://github.com/user-attachments/assets/1fe18849-73b7-4a29-ad92-ceca3d5778d7" />

## Installation

### Prerequisites

- GCC or Clang, and Make
- OpenSSL development headers (libcrypto)
- Ncurses development headers (libncursesw)
- Unzip (for extracting the bundled SQLCipher source)

### Building and Installing

To build and install Jailcell, run the following commands:

```bash
make
sudo make install
```

To build using Clang instead of the default GCC:

```bash
make CC=clang
```

By default, the binary is installed to `/usr/local/bin` and the man page to `/usr/local/share/man/man1`. You can customize the installation prefix:

```bash
make PREFIX=/usr/local install
```

### Uninstalling

To remove the installed binary and man page:

```bash
sudo make uninstall
```

## Usage

```bash
jailcell [options] <cell_name>
```

### Options
- `-n, --new`: Create a new encrypted cell.
- `-a, --add`: Add a new text note (requires `-t` and `-c`).
- `-t, --title <text>`: Set the title for a note.
- `-c, --content <text>`: Set the content for a note.
- `-F, --file <path>`: Import a file into the cell.
- `-l, --list`: List all notes and files.
- `-s, --show <id>`: Display note content or file metadata.
- `-f, --format <fmt>`: Set output format (`text`, `json`, or `xml`).
- `-x, --export <id>`: Export a file from the cell (requires `-o`).
- `-o, --out <path>`: Destination path for exported files.
- `-v, --version`: Show version information.
- `-h, --help`: Show help message.

Jailcell launches in interactive TUI mode only when a cell name is provided without any action flags. If action flags (such as --list, --add, or --show) are present, Jailcell executes the specific command and exits immediately.

## TUI Mode

Jailcell includes an interactive TUI for managing your cell without manual CLI flags. It uses a modal system inspired by Vim.

### Controls
- **Normal Mode**: Navigation and action triggers.
    - `h`, `j`, `k`, `l` / Arrows: Movement.
    - `w`, `b`: Jump by word.
    - `{`, `}`: Jump by paragraph.
    - `i`, `I`, `A`, `C`: Enter Insert mode (standard Vim behavior).
    - `D`: Delete to end of line.
    - `Ctrl+P`: Open Item Finder/Search.
    - `:`: Enter Command mode.
- **Command Mode**:
    - `:w`: Save changes to the cell.
    - `:q`: Quit the application.
    - `:wq`: Save and quit.
    - `:new <title>`: Create a new note.
    - `:rename <name>`: Rename the current item.
    - `:delete`: Delete the current item.
    - `:export <path>`: Export current item to disk.
- **Finder Mode (Ctrl+P)**:
    - Type to filter items by name.
    - `j`/`k` to select, `Enter` to open.

## Examples

```bash
# Create a new encrypted database
jailcell -n myvault.db

# Add a note with a title and content
jailcell -a -t "Meeting" -c "Project plan approved." myvault.db

# Import a binary file
jailcell -F ./document.pdf myvault.db

# List all entries in a table format
jailcell -l myvault.db

# List all entries as JSON
jailcell -l -f json myvault.db

# List all entries as XML
jailcell -l -f xml myvault.db

# Show content of note with ID 1
jailcell -s 1 myvault.db

# Get JSON metadata for item with ID 2
jailcell -s 2 -f json myvault.db

# Get XML metadata for item with ID 2
jailcell -s 2 -f xml myvault.db

# Export file with ID 3 to the local filesystem
jailcell -x 3 -o ./recovered.pdf myvault.db

# Launch the interactive TUI
jailcell myvault.db
```

## Security Considerations

- **Encryption**: Jailcell uses AES-256 encryption provided by SQLCipher. The database is encrypted at rest.
- **Memory Safety**: Master passwords are wiped from memory using `memset` immediately after the database is opened. However, sensitive data currently being displayed in the TUI or terminal may remain in system RAM until the process terminates.
- **TUI Session**: The TUI does not automatically lock. It is recommended to close the application or lock your screen when leaving your terminal unattended.
- **Binary Files**: When exporting files, they are written to the disk in their original (unencrypted) form. Ensure the destination directory is secure.

## Acknowledgements

Jailcell makes use of the following libraries:
- [ezxml](https://github.com/lxfontes/ezxml) - XML parsing library.
- [cJSON](https://github.com/DaveGamble/cJSON) - JSON parsing library.
