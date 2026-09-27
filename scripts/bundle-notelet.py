#!/usr/bin/env python3
"""Build a deterministic, uncompressed ustar .notelet bundle."""
import argparse
import io
from pathlib import Path
import re
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="directory containing main.tsc")
    parser.add_argument("output", type=Path, help="destination NAME.notelet")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9_-]{1,64}", args.output.stem) or args.output.suffix != ".notelet":
        parser.error("output name must be [a-z0-9_-]+.notelet")
    library = Path(__file__).resolve().parent.parent / "notelets/notelet.tsc"
    main_file = args.source / "main.tsc"
    if not main_file.is_file() or main_file.is_symlink():
        parser.error("source must contain a regular main.tsc")
    entries = [("main.tsc", main_file), ("notelet.tsc", library)]
    assets = args.source / "assets"
    if assets.exists():
        for file in sorted(assets.rglob("*")):
            if file.is_symlink():
                parser.error(f"symlink not supported: {file}")
            if file.is_file():
                entries.append(("assets/" + file.relative_to(assets).as_posix(), file))
    with tarfile.open(args.output, "w", format=tarfile.USTAR_FORMAT) as archive:
        for name, path in entries:
            content = path.read_bytes()
            if len(content) > 1024 * 1024 or len(name) > 99:
                parser.error(f"entry too large or path too long: {name}")
            info = tarfile.TarInfo(name)
            info.size = len(content)
            info.mtime = 0
            info.mode = 0o644
            info.uid = info.gid = 0
            archive.addfile(info, io.BytesIO(content))
    if args.output.stat().st_size > 4 * 1024 * 1024:
        args.output.unlink()
        parser.error("bundle exceeds 4 MiB")


if __name__ == "__main__":
    main()
