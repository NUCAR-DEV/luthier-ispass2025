# !/usr/bin/env python3
import os.path
import argparse
from typing import Tuple
import pickle

from common import read_yaml_cfg, capture_subprocess_output


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("Instruction count runner script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")
    parser.add_argument("--luthier_instr_count_tool_path", type=str,
                        default="/Luthier/build/examples/InstrCount/libLuthierInstrCount.so",
                        help="location of the luthier instruction count tool")
    parser.add_argument("--nvbit_instr_count_tool_path", type=str,
                        default="/nvbit_release/tools/instr_count/instr_count.so",
                        help="location of the nvbit instruction count tool")
    parser.add_argument("--dump_stdout_stderr", action="store_true",
                        help="dump the stdout and stderr of each experiment to screen")
    parser.add_argument("--overwrite_results", action="store_true",
                        help="overwrite results of benchmarks from a previous run")

    args = parser.parse_args()
    return args

RESULT_PKLE_FILE_NAME = "instrcount-results.pkl"

def luthier_get_instr_count_tool_results(stderr: str) -> Tuple[int, int]:
    stderr_lines = stderr.splitlines()
    instruction_count = -1
    kernel_runtime = -1
    for line in reversed(stderr_lines):
        if instruction_count != -1 and kernel_runtime != -1:
            break
        if line.find("Total number of counted instructions:") != -1:
            instruction_count = int(line.replace("Total number of counted instructions:", "").replace(".", ""))
        elif line.find("Total kernel run time (us):") != -1:
            kernel_runtime = int(line.replace("Total kernel run time (us):", "").replace(".", ""))
    if instruction_count == -1:
        raise EOFError("Failed to find the instruction count.")
    if kernel_runtime == -1:
        raise EOFError("Failed to find the kernel runtime.")
    return instruction_count, kernel_runtime


def nvbit_get_instr_count_tool_results(stdout: str, stderr: str) -> Tuple[int, int]:
    out_lines = stdout.splitlines() + stderr.splitlines()
    instruction_count = -1
    kernel_runtime = -1
    for line in reversed(out_lines):
        if instruction_count != -1 and kernel_runtime != -1:
            break
        if line.find("Total app instructions:") != -1:
            instruction_count = int(line.replace("Total app instructions:", "").replace(".", ""))
        elif line.find("Total kernel time:") != -1:
            kernel_runtime = int(line.replace("Total kernel time:", "").replace(".", ""))
    if instruction_count == -1:
        raise EOFError("Failed to find the instruction count.")
    if kernel_runtime == -1:
        raise EOFError("Failed to find the kernel runtime.")
    return instruction_count, kernel_runtime


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()
    for bench in benchmark_cfg["InstrCount"]["benchmarks"]:
        for programming_model in benchmark_cfg["InstrCount"]["programming_models"]:
            out = {}
            cfgs = benchmark_cfg["HeCBench"][bench]["programming_models"][programming_model]
            run_flags = eval(cfgs["run_command"])
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            if os.path.exists(os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME)) and not args.overwrite_results:
                print(f"Skipping {bench}-{programming_model} as existing results were found.")
                continue
            env_vars = os.environ.copy()
            # Run instrumented once to get both the instruction count and the kernel runtime overhead

            # Set the environment variables
            if programming_model == "cuda":
                tool_path = args.nvbit_instr_count_tool_path
                env_vars["LD_PRELOAD"] = tool_path
                env_vars["COUNT_WARP_LEVEL"] = "0"
                env_vars["MANGLED_NAMES"] = "0"
            else:
                tool_path = args.luthier_instr_count_tool_path
                env_vars["LD_PRELOAD"] = tool_path
                env_vars["HIP_ENABLE_DEFERRED_LOADING"] = "0"
            print(f"Running instruction count tool on {bench}-{programming_model} with command {' '.join(run_flags)}")
            # Run the program
            return_code, stdout, stderr = capture_subprocess_output(args=run_flags, cwd=benchmark_folder, env=env_vars,
                                                                    dump_stdout_stderr=args.dump_stdout_stderr)
            if return_code:
                raise ChildProcessError(f"Failed to run the instrumented version of {bench}-{programming_model}.")

            if programming_model == "cuda":
                instr_count, kernel_time = nvbit_get_instr_count_tool_results(stdout, stderr)
            else:
                instr_count, kernel_time = luthier_get_instr_count_tool_results(stderr)
            out["Number of Instructions"] = instr_count
            out["Instrumented Kernel Runtime (us)"] = kernel_time
            print(
                f"Running the un-instrumented version of {bench}-{programming_model} with command {' '.join(run_flags)}")
            # Run another time un-instrumented to get the original kernel runtime
            if programming_model == "cuda":
                env_vars["INSTR_END"] = "0"
            else:
                env_vars["LUTHIER_ARGS"] = "--instr-end-interval=0"
            return_code, stdout, stderr = capture_subprocess_output(args=run_flags, cwd=benchmark_folder, env=env_vars,
                                                                    dump_stdout_stderr=args.dump_stdout_stderr)
            if return_code:
                raise ChildProcessError(f"Failed to run the un-instrumented version of {bench}-{programming_model}.")
            if programming_model == "cuda":
                instr_count, kernel_time = nvbit_get_instr_count_tool_results(stdout, stderr)
            else:
                instr_count, kernel_time = luthier_get_instr_count_tool_results(stderr)
            out["Un-instrumented Kernel Runtime (us)"] = kernel_time
            print(out)
            with open(os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME), "wb") as f:
                pickle.dump(out, f, protocol=pickle.HIGHEST_PROTOCOL)
        print(f"Ran {bench} benchmarks.")


if __name__ == "__main__":
    main()
