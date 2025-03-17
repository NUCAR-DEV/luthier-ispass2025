import math
from common import read_yaml_cfg
import argparse
import os
import pickle


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("Instruction count runner script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")

    args = parser.parse_args()
    return args


RESULT_PKLE_FILE_NAME = "instrcount-results.pkl"


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()

    # Read in the plot entries
    plot_entries = {programming_model: [] for programming_model in benchmark_cfg["InstrCount"]["programming_models"]}
    for bench in benchmark_cfg["InstrCount"]["benchmarks"]:
        for programming_model in benchmark_cfg["InstrCount"]["programming_models"]:
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            with open(os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME), 'rb') as f:
                data_point = pickle.load(f)
                plot_entries[programming_model].append(data_point["Number of Instructions"])

    print("Instruction Counts:")
    for idx, bench in enumerate(benchmark_cfg["InstrCount"]["benchmarks"]):
        print(f"- {bench}:")
        for programming_model in benchmark_cfg["InstrCount"]["programming_models"]:
            print(
                f"\t-{programming_model}: {plot_entries[programming_model][idx]} (~= 10^{math.log10(plot_entries[programming_model][idx])})")


if __name__ == "__main__":
    main()
