import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

def restore(output=None):
    root = Path(__file__).resolve().parent
    parts = root / "mechanism_samples.parts"
    manifest = json.loads((parts / "manifest.json").read_text(encoding="utf-8"))
    destination = Path(output) if output else root / manifest["filename"]
    if destination.exists():
        with destination.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if destination.stat().st_size != manifest["size"] or digest != manifest["sha256"]:
            raise ValueError("Existing output differs from the manifest")
        print("Verified existing file", destination)
        return
    descriptor, temporary = tempfile.mkstemp(prefix=".restore-", dir=destination.parent)
    try:
        whole = hashlib.sha256()
        size = 0
        with os.fdopen(descriptor, "wb") as target:
            for entry in manifest["parts"]:
                part_hash = hashlib.sha256()
                part_size = 0
                with (parts / entry["name"]).open("rb") as source:
                    while chunk := source.read(1024 * 1024):
                        target.write(chunk)
                        whole.update(chunk)
                        part_hash.update(chunk)
                        size += len(chunk)
                        part_size += len(chunk)
                if part_size != entry["size"] or part_hash.hexdigest() != entry["sha256"]:
                    raise ValueError("Invalid part " + entry["name"])
        if size != manifest["size"] or whole.hexdigest() != manifest["sha256"]:
            raise ValueError("Reconstructed file failed verification")
        if destination.exists():
            raise FileExistsError(destination)
        os.replace(temporary, destination)
        print("Restored and verified", destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Restore the original B0 compressed CSV from verified parts")
    parser.add_argument("--output", type=Path)
    restore(parser.parse_args().output)
