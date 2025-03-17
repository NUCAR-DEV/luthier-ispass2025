# !/usr/bin/env python3
import os.path
import argparse
from typing import Dict
import pickle

from common import read_yaml_cfg, capture_subprocess_output


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("Opcode histogram runner script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")
    parser.add_argument("--luthier_opcode_histogram_tool_path", type=str,
                        default="/Luthier/build/examples/OpcodeHistogram/libLuthierOpcodeHistogram.so",
                        help="location of the luthier instruction count tool")
    parser.add_argument("--dump_stdout_stderr", action="store_true",
                        help="dump the stdout and stderr of each experiment to screen")
    parser.add_argument("--overwrite_results", action="store_true",
                        help="overwrite results of benchmarks from a previous run")

    args = parser.parse_args()
    return args

RESULT_PKLE_FILE_NAME = "opcode-histogram-results.pkl"

def luthier_get_opcode_histogram_tool_results(stderr: str) -> Dict[str, int]:
    stderr_lines = stderr.splitlines()
    instr_count_line_idx = -1
    opcode_histogram_line_idx = -1
    for idx, line in reversed(list(enumerate(stderr_lines))):
        if instr_count_line_idx != -1 and opcode_histogram_line_idx != -1:
            break
        if line.find("Total number of instructions counted:") != -1:
            instr_count_line_idx = idx
        elif line.find("Instruction Opcode Results:") != -1:
            opcode_histogram_line_idx = idx
    if instr_count_line_idx == -1:
        raise EOFError("Failed to find the instruction count line index.")
    if opcode_histogram_line_idx == -1:
        raise EOFError("Failed to find the opcode histogram line index")
    out = {}

    for entry in stderr_lines[opcode_histogram_line_idx + 1: instr_count_line_idx]:
        k, v = entry.split(" = ")
        out[k.strip()] = int(v)
    return out


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()
    for bench in benchmark_cfg["OpcodeHistogram"]["benchmarks"]:
        for programming_model in benchmark_cfg["OpcodeHistogram"]["programming_models"]:
            cfgs = benchmark_cfg["HeCBench"][bench]["programming_models"][programming_model]
            run_flags = eval(cfgs["run_command"])
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            if os.path.exists(os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME)) and not args.overwrite_results:
                print(f"Skipping {bench}-{programming_model} as existing results were found.")
                continue
            env_vars = os.environ.copy()
            # Set the environment variables
            tool_path = args.luthier_opcode_histogram_tool_path
            env_vars["LD_PRELOAD"] = tool_path
            env_vars["HIP_ENABLE_DEFERRED_LOADING"] = "0"
            print(f"Running the opcode histogram tool on {bench}-{programming_model} with command {' '.join(run_flags)}")
            # Run the program
            return_code, stdout, stderr = capture_subprocess_output(args=run_flags, cwd=benchmark_folder, env=env_vars,
                                                                    dump_stdout_stderr=args.dump_stdout_stderr)
            if return_code:
                raise ChildProcessError(f"Failed to run the opcode histogram tool on {bench}-{programming_model}.")

            out = luthier_get_opcode_histogram_tool_results(stderr)
            print(out)
            with open(os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME), "wb") as f:
                pickle.dump(out, f, protocol=pickle.HIGHEST_PROTOCOL)
        print(f"Ran {bench} benchmarks.")


if __name__ == "__main__":
    main()
