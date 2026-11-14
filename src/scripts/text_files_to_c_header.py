#!/usr/bin/env python3
"""Convert text files in a directory into C strings in a header.

Usage: python3 embed_strings.py INPUT_DIRECTORY OUTPUT_HEADER
"""

import argparse
from pathlib import Path
import re


def escape_c_string(text):
    """Escape characters that cannot appear literally inside a C string."""
    # Escape existing backslashes first, before adding any new ones.
    text = text.replace("\\", "\\\\")
    text = text.replace('"', '\\"')
    text = text.replace("\t", "\\t")
    text = text.replace("\r", "\\r")
    text = text.replace("\n", "\\n")
    return text


def generate_header(input_directory, output_file):
    declarations = []
    used_names = set()

    for input_file in sorted(input_directory.iterdir()):
        # For example, "many-tris.vert" becomes "many_tris_vert".
        variable_name = re.sub(r"[^A-Za-z0-9_]", "_", input_file.name)
        if variable_name in used_names:
            raise ValueError(f"multiple files produce the name: {variable_name}")
        used_names.add(variable_name)

        # Preserve the original line endings rather than converting them.
        with input_file.open(encoding="utf-8", newline="") as source:
            contents = source.read()

        for character in contents:
            if ord(character) < 32 and character not in "\t\r\n":
                raise ValueError(f"unsupported control character in {input_file}")

        declaration = f"static const char {variable_name}[] =\n"

        # Adjacent C string literals are joined into one string by the compiler.
        source_lines = contents.splitlines(keepends=True)
        if not source_lines:
            source_lines = [""]

        string_literals = []
        for source_line in source_lines:
            escaped_line = escape_c_string(source_line)
            string_literals.append(f'    "{escaped_line}"')

        declaration += "\n".join(string_literals) + ";\n"
        declarations.append(declaration)

    return "\n".join(declarations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_directory", type=Path)
    parser.add_argument("output_file", type=Path)
    args = parser.parse_args()

    try:
        header = generate_header(args.input_directory, args.output_file)

        if args.output_file.exists():
            old_header = args.output_file.read_text(encoding="utf-8")
        else:
            old_header = None

        if header != old_header:
            args.output_file.write_text(header, encoding="utf-8")

    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
