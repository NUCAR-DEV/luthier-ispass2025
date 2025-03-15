# !/usr/bin/env python3
import os.path
import subprocess
import argparse
from benchmark_cfg import read_yaml_cfg


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("HeC benchmark compilation script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")
    parser.add_argument("--action", type=str, choices=["clean", "build"], default="build",
                        help="The action to perform")
    args = parser.parse_args()
    return args


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()["HeCBench"]
    for bench, programming_models in benchmark_cfg.items():
        for programming_model, cfgs in programming_models["programming_models"].items():
            print(cfgs)
            compile_flags = eval(cfgs["compilation_flags"])
            print(f"{args.action.capitalize()}ing {bench}-{programming_model}")
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            make_command = ["make"] + list(compile_flags) if args.action == "build" else ["make", "clean"]
            status_code = subprocess.call(args=make_command, cwd=benchmark_folder)
            if status_code:
                raise ChildProcessError(f"Failed {args.action}ing {bench}-{programming_model}.")
        print(f"Compiled {bench} benchmarks.")


if __name__ == "__main__":
    main()
