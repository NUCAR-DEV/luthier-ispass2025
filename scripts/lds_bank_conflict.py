# !/usr/bin/env python3
import os.path
import argparse
from typing import Tuple
import pickle

from common import read_yaml_cfg, capture_subprocess_output


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("LDS bank conflict runner script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")
    parser.add_argument("--luthier_lds_bank_conflict_tool_path", type=str,
                        default="/Luthier/build/examples/LDSBankConflict/libLuthierLDSBankConflict.so",
                        help="location of the luthier instruction count tool")
    parser.add_argument("--dump_stdout_stderr", action="store_true",
                        help="dump the stdout and stderr of each experiment to screen")
    parser.add_argument("--overwrite_results", action="store_true",
                        help="overwrite results of benchmarks from a previous run")

    args = parser.parse_args()
    return args


RESULT_PKLE_FILE_NAME = "lds-bank-conflict-results.pkl"


def luthier_get_lds_bank_conflict_tool_results(stderr: str) -> Tuple[int, int]:
    stderr_lines = stderr.splitlines()
    lds_count = -1
    bank_conflict = -1
    for line in reversed(stderr_lines):
        if lds_count != -1 and bank_conflict != -1:
            break
        if line.find("Total number of LDS instructions:") != -1:
            lds_count = int(line.replace("Total number of LDS instructions:", "").strip())
        elif line.find("Total number of bank conflicts detected:") != -1:
            bank_conflict = int(line.replace("Total number of bank conflicts detected:", "").strip())
    if lds_count == -1:
        raise EOFError("Failed to find the LDS instruction count.")
    if bank_conflict == -1:
        raise EOFError("Failed to find the number of bank conflicts.")
    return lds_count, bank_conflict


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()
    for bench in benchmark_cfg["LDS"]["benchmarks"]:
        for programming_model in benchmark_cfg["LDS"]["programming_models"]:
            if programming_model not in  benchmark_cfg["HeCBench"][bench]["programming_models"]:
                continue
            cfgs = benchmark_cfg["HeCBench"][bench]["programming_models"][programming_model]
            run_flags = eval(cfgs["run_command"])
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            if os.path.exists(os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME)) and not args.overwrite_results:
                print(f"Skipping {bench}-{programming_model} as existing results were found.")
                continue
            env_vars = os.environ.copy()
            # Set the environment variables
            tool_path = args.luthier_lds_bank_conflict_tool_path
            env_vars["LD_PRELOAD"] = tool_path
            env_vars["HIP_ENABLE_DEFERRED_LOADING"] = "0"
            print(
                f"Running the LDS bank conflict tool on {bench}-{programming_model} with command {' '.join(run_flags)}")
            # Run the program
            return_code, stdout, stderr = capture_subprocess_output(args=run_flags, cwd=benchmark_folder, env=env_vars,
                                                                    dump_stdout_stderr=args.dump_stdout_stderr)
            if return_code:
                raise ChildProcessError(f"Failed to run the LDS bank conflict tool on {bench}-{programming_model}.")

            print(stderr)
            lds_count, num_bank_conflicts = luthier_get_lds_bank_conflict_tool_results(stderr)
            out = {"Number of LDS instructions": lds_count, "Number of Bank Conflicts": num_bank_conflicts}
            print(out)
            with open(os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME), "wb") as f:
                pickle.dump(out, f, protocol=pickle.HIGHEST_PROTOCOL)
        print(f"Ran {bench} benchmarks.")


if __name__ == "__main__":
    main()
