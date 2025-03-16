# !/usr/bin/env python3
import os.path
import argparse

from common import read_yaml_cfg, capture_subprocess_output


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("HeC benchmark compilation script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")
    parser.add_argument("--luthier_instr_count_tool_path", type=str,
                        default="/work/Luthier/build/examples/InstrCount/libLuthierInstrCount.so",
                        help="location of the luthier instruction count tool")
    parser.add_argument("--nvbit_instr_count_tool_path", type=str,
                        default="/work/nvbit_release/tools/instr_count/instr_count.so",
                        help="location of the nvbit instruction count tool")

    args = parser.parse_args()
    return args


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()
    for bench in benchmark_cfg["InstrCount"]["benchmarks"]:
        for programming_model, cfgs in benchmark_cfg["HeCBench"][bench]["programming_models"].items():
            run_flags = eval(cfgs["run_command"])
            print(f"Running instruction count on {bench}-{programming_model}")
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            env_vars = os.environ.copy()
            if programming_model == "cuda":
                tool_path = args.nvbit_instr_count_tool_path
                env_vars["LD_PRELOAD"] = tool_path
                env_vars["COUNT_WARP_LEVEL"] = "0"
                env_vars["MANGLED_NAMES"] = "0"
                print(run_flags, benchmark_folder)
            else:
                tool_path = args.luthier_instr_count_tool_path
                env_vars["LD_PRELOAD"] = tool_path
            return_code, stdout, stderr = capture_subprocess_output(args=run_flags, cwd=benchmark_folder, env=env_vars)
            if return_code:
                raise ChildProcessError(f"Failed running instruction count tool on {bench}-{programming_model}.")
        print(f"Compiled {bench} benchmarks.")


if __name__ == "__main__":
    main()
