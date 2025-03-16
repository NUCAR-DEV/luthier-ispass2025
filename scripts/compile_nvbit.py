# !/usr/bin/env python3
import subprocess
import argparse


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("HeC benchmark compilation script")
    parser.add_argument("--nvbit_dir", type=str,
                        default="/work/nvbit_release",
                        help="location of the HeCBench repo")
    parser.add_argument("--action", type=str, choices=["clean", "build"], default="build",
                        help="The action to perform")
    args = parser.parse_args()
    return args


def main():
    args = parse_and_validate_args()
    return_code = subprocess.call(args="make" if args.action == "build" else "make clean",
                                  cwd="/nvbit_release/tools/instr_count")
    if not return_code:
        raise ChildProcessError(f"Failed to {args.action} NVBit instruction counter tool.")
    print(f"Done.")


if __name__ == "__main__":
    main()
