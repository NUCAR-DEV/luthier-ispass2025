import math
from common import read_yaml_cfg
import argparse
import os
import pickle


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("LDS bank conflict printer script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")

    args = parser.parse_args()
    return args


RESULT_PKLE_FILE_NAME = "lds-bank-conflict-results.pkl"


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()

    # Read in the plot entries
    plot_entries = {bench: {} for bench in benchmark_cfg["LDS"]["benchmarks"]}
    for bench in benchmark_cfg["LDS"]["benchmarks"]:
        for programming_model in benchmark_cfg["LDS"]["programming_models"]:
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            file_name = os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME)
            if os.path.exists(file_name):
                with open(file_name, 'rb') as f:
                    data_point = pickle.load(f)
                    plot_entries[bench][programming_model] = data_point
    print("LDS Access Stats:")
    for bench in plot_entries:
        for programming_model in plot_entries[bench]:
            print(f"- {bench}, {programming_model}:")
            for stat in plot_entries[bench][programming_model]:
                res = plot_entries[bench][programming_model][stat]
                if res:
                    print(
                        f"\t-{stat}: {res} (~= 10^{math.log10(res)})")
                else:
                    print(f"\t-{stat}: {res})")


if __name__ == "__main__":
    main()
