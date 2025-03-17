from common import read_yaml_cfg
import argparse
import os
import pickle


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("Opcode histogram printer script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")

    args = parser.parse_args()
    return args


RESULT_PKLE_FILE_NAME = "opcode-histogram-results.pkl"


def main():
    args = parse_and_validate_args()
    benchmark_cfg = read_yaml_cfg()

    # Read in the plot entries
    plot_entries = {programming_model: {} for programming_model in benchmark_cfg["OpcodeHistogram"]["programming_models"]}
    for bench in benchmark_cfg["OpcodeHistogram"]["benchmarks"]:
        for programming_model in benchmark_cfg["OpcodeHistogram"]["programming_models"]:
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            file_name = os.path.join(benchmark_folder, RESULT_PKLE_FILE_NAME)
            if os.path.exists(file_name):
                with open(file_name, 'rb') as f:
                    data_point = pickle.load(f)
                    for opcode, count in data_point.items():
                        if opcode in plot_entries[programming_model]:
                            plot_entries[programming_model][opcode] += count
                        else:
                            plot_entries[programming_model][opcode] = count
    print("Top 5 instructions executed in:")
    for programming_model, stats in plot_entries.items():
        print(f"- {programming_model}:")
        sorted_stats = sorted(stats.items(), key=lambda x: x[1], reverse=True)
        total = sum(stats.values())
        for opcode, count in sorted_stats[:5]:
            print(f"\t{opcode} : {count / total * 100:.2f}%")


if __name__ == "__main__":
    main()
