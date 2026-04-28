"""
runner.py — Top-level CLI for automation tests.

Usage:
  python runner.py sort_the_court [--rounds N]
  python runner.py sort_the_court --rounds 5
"""

import argparse
import sys
import os
from pathlib import Path

# Ensure automation/ is on the path so submodules can import capture/process/input
sys.path.insert(0, os.path.dirname(__file__))


def cmd_sort_the_court(args):
    from games.sort_the_court import run
    result = run(rounds=args.rounds, verbose=True)
    print(f"\n=== RESULT ===")
    print(f"Rounds played : {result['rounds_played']}")
    print(f"Screenshots   : {len(result['screenshots'])}")
    print(f"Error         : {result['error']}")

    # Save screenshots for inspection
    out_dir = Path(r"S:\bld\vboxgpu\automation_out")
    out_dir.mkdir(exist_ok=True)
    for i, img in enumerate(result["screenshots"]):
        p = out_dir / f"sc_frame_{i:02d}.png"
        img.save(p)
        print(f"  saved: {p}")

    return 0 if result["error"] is None else 1


def main():
    parser = argparse.ArgumentParser(description="VBox GPU Bridge automation runner")
    sub = parser.add_subparsers(dest="game", required=True)

    sc = sub.add_parser("sort_the_court", help="Run Sort the Court automation")
    sc.add_argument("--rounds", type=int, default=5, help="Number of rounds to play")

    args = parser.parse_args()

    if args.game == "sort_the_court":
        sys.exit(cmd_sort_the_court(args))


if __name__ == "__main__":
    main()
