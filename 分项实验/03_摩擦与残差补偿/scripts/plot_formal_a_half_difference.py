"""Regenerate the formal A figure with the workspace MATLAB drawing standard."""
from pathlib import Path
import subprocess


if __name__ == "__main__":
    subprocess.run(
        ["matlab", "-batch", "plot_formal_a_half_difference"],
        cwd=Path(__file__).resolve().parent,
        check=True,
    )
