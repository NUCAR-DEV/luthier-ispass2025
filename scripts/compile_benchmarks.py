# !/usr/bin/env python3
import os.path
import subprocess
import argparse

HIP_HEC_BENCHMARKS = (
    "accuracy", "ace", "adam", "burger", "maxpool3d", "convolutionSeparable", "dense-embedding", "winograd",
    "atomicReduction", "matrix-rotate", "eigenvalue", "b+tree", "floydwarshall", "fpc", "hwt1d", "nn", "hmm",
    "inversek2j", "heat", "streamcluster", "stencil3d", "spm", "hexciton", "rotary", "histogram", "zoom", "damage",
    "tensorT", "rowwiseMoments", "spgemm")

HIP_BUILD_COMMAND_FLAGS = ["-e",
                           'EXTRA_CFLAGS=-Xoffload-linker \"--emit-relocs\"']

OPENMP_HEC_BENCHMARKS = (
    "accuracy", "ace", "adam", "burger", "maxpool3d", "convolutionSeparable", "dense-embedding", "winograd",
    "atomicReduction", "matrix-rotate", "eigenvalue", "b+tree", "floydwarshall", "fpc", "hwt1d", "nn", "hmm",
    "inversek2j", "heat", "streamcluster", "stencil3d", "spm")

OPENMP_BUILD_COMMAND_FLAGS = ["-e", "CC=/opt/rocm/llvm/bin/clang++", "-e",
                              'EXTRA_CFLAGS="-Xoffload-linker --emit-relocs -O3"', "-e", "ARCH=gfx908", "-f",
                              "Makefile.aomp"]

SYCL_HEC_BENCHMARKS = (
    "accuracy", "ace", "adam", "burger", "maxpool3d", "convolutionSeparable", "dense-embedding", "winograd",
    "atomicReduction", "matrix-rotate", "eigenvalue", "b+tree", "floydwarshall", "fpc", "hwt1d", "nn", "hmm",
    "inversek2j", "heat", "streamcluster", "stencil3d", "spm", "hexciton", "rotary", "histogram", "zoom", "damage",
    "tensorT", "rowwiseMoments", "spgemm")

SYCL_BUILD_COMMAND_FLAGS = ["-e", "GCC_TOOLCHAIN=/usr/", "-e", "CC=/opt/sycl/llvm/bin/clang++", "-e", "HIP=yes", "-e",
                            "HIP_ARCH=gfx908"]


def parse_and_validate_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser("HeC benchmark compilation script")
    parser.add_argument("--hecbench_dir", type=str,
                        default="/work/HeCBench",
                        help="location of the HeCBench repo")
    parser.add_argument("--action", type=str, choices=["clean", "build"], default="build")
    args = parser.parse_args()
    return args


def main():
    args = parse_and_validate_args()

    for programming_model, benchmark_list, compile_flags in zip(("hip",
                                                                 "omp",
                                                                 "sycl"),
                                                                (HIP_HEC_BENCHMARKS,
                                                                 OPENMP_HEC_BENCHMARKS,
                                                                 SYCL_HEC_BENCHMARKS),
                                                                (HIP_BUILD_COMMAND_FLAGS,
                                                                 OPENMP_BUILD_COMMAND_FLAGS,
                                                                 SYCL_BUILD_COMMAND_FLAGS)):
        print(f"{args.action.capitalize()}ing {programming_model} HeC benchmarks...")
        for bench in benchmark_list:
            print(f"{args.action.capitalize()}ing {bench}-{programming_model}")
            benchmark_folder = os.path.join(args.hecbench_dir, "src", f"{bench}-{programming_model}")
            make_command = ["make"] + compile_flags if args.action == "build" else ["make", "clean"]
            status_code = subprocess.call(args=make_command, cwd=benchmark_folder)
            if status_code:
                raise ChildProcessError(f"Failed {args.action}ing {bench}-{programming_model}.")
        print(f"Compiled {programming_model} HeC benchmarks.")


if __name__ == "__main__":
    main()
