# Info Paster

A minimal C99 TUI tool that reads personal information from a JSON file and copies selected values to the system clipboard with a single keypress.

## Build & Run

```bash
./build.sh           # Compiles to bin/info_paster
./bin/info_paster    # Uses ./info.json by default
./bin/info_paster ~/my_info.json  # Or specify a custom path
```

## Setup

Create an `info.json` file at the project root with your personal data:

```json
{
    "Full Name":  "John Doe",
    "Email":      "john@example.com",
    "Phone":      "+1-555-0123",
    "LinkedIn":   "https://linkedin.com/in/johndoe"
}
```

Each key-value pair becomes a selectable entry in the TUI. You can add as many fields as you want.

## Usage

Each entry is assigned a shortcut key derived from its label (e.g., `f` for "Full Name", `e` for "Email"). Press the key to instantly copy the value to your clipboard.

```
  INFO PASTER
  Press a key to copy to clipboard. 'q' to quit.

  ──────────────────

  [f]  Full Name   John Doe
  [e]  Email       ✓ Copied!
  [p]  Phone       +1-555-0123
  [l]  LinkedIn    https://linkedin.com/in/johndoe

  ──────────────────
```

If two labels start with the same letter, they get two-character shortcuts (e.g., `gi` and `gp` for "GitHub" and "GPA").

## Dependencies

- A C99 compiler (gcc)
- One of: `xclip`, `xsel`, or `wl-copy` for clipboard access
