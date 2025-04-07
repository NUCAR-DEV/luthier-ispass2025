# Artifact for ISPASS 2025 Paper "Luthier: A Dynamic Binary Instrumentation Framework Targeting AMD GPUs"

This repository contains the artifact for the paper
"Luthier: A Dynamic Binary Instrumentation Framework Targeting AMD GPUs" accepted to 2025 IEEE International Symposium 
on Performance Analysis of Systems and Software (ISPASS '25).

## Contents
1. A [Dockerfile](./Dockerfile) used to build the environment for the paper experiments.
2. A snapshot of the [Luthier project](https://github.com/matinraayai/Luthier) under the 
   [`Luthier/`](./Luthier) folder, with git revision number `6a1ae19b62ea9d4b021e4555c55a02b5bd1a885a`.
3. A snapshot of the [HeCBench repository](https://github.com/zjin-lcf/HeCBench) used to obtain the figures in 
   the paper under the [`HecBench`](./HeCBench) folder, with git revision 
   number `b59cdcc3755c3a0cd39b4b9925ac5aa76b1d1171`.
4. A snapshot of [NVBit](https://github.com/NVlabs/NVBit) version 1.7.4, under the [`nvbit_release`](./nvbit_release)
   folder. The instruction counter example tool has been modified to measure the host runtime of the instrumented 
   kernels.
5. A set of Python scripts used to run the experiments and obtain the results shown in the figures in text format.

## Requirements
1. An NVIDIA V100 and an AMD MI100 GPU attached to the same system.
2. NVIDIA Kernel Driver (v560.35.03 was used).
3. NVIDIA Container toolkit (v1.16.2 was used).
4. AMDGPU Kernel Driver (v.6.2.3 was used).
5. Docker Runtime (v27.3.1 was used).
6. A Linux OS (Ubuntu 22.04.5 LTS was used).

The container itself contains the following pre-requisite software:
1. NVIDIA CUDA toolkit version 12.8.0.
2. GNU C/C++ Compiler version 12.
3. Python 3 with packages `cxxheaderparser`, `pcpp`, and `yacs`.
4. CMake version 3.28.3.
5. [Bleeding-edge ROCm compilation software stack](https://github.com/ROCm/llvm-project) obtained by from the 
   `amd-staging` branch of the LLVM ROCm fork.
6. [Intel SYCL compiler](https://github.com/intel/llvm).

The Git revision number of all software is included inside the `Dockerfile`.
## How To Run
1. Clone this repository, and change to the repository directory:
   ```bash
   git clone --single-branch --depth 1 https://github.com/NUCAR-DEV/luthier-ispass2025
   cd luthier-ispass20205
   ```
2. Either build the container from scratch:
   ```bash
   docker build --tag=containers.rc.northeastern.edu/luthier/ispass-2025-artifact --force-rm .
   ```
   Or pull an already built version:
   ```bash
   docker pull containers.rc.northeastern.edu/luthier/ispass-2025-artifact
   ```
3. Run the container via the following command:
   ```bash
   docker run --rm -it -v $PWD:/work/  --device=/dev/kfd --device=/dev/dri/ --privileged \
   --security-opt seccomp=unconfined --shm-size=16G --cap-add=SYS_PTRACE --ipc=host --privileged --gpus all \
   containers.rc.northeastern.edu/luthier/ispass-2025-artifact /bin/bas
   ```
4. Build the HeC benchmarks:
   ```bash
   python3 scripts/compile_benchmarks.py --action build
   ```
5. To run the experiments, run the following scripts:
   ```bash
   # For figure 4(a) and 4(c)
   python3 scripts/instr_count.py --dump_stdout_stderr
   # For figure 5
   python3 scripts/opcode_histogram.py --dump_stdout_stderr
   # For figure 4(b)
   python3 scripts/lds lds_bank_conflict.py --dump_stdout_stderr
   ```
   Note that the `--dump_stdout_stderr` dumps the output of each experiments to the standard output/error, which 
   can be quite large; Therefore, it is recommended to clip the terminal emulator output when running the experiments.
6. To print the results, run the following scripts:
   ```bash
   # Figure 4(a)
   python3 scripts/print_instr_count.py
   # For figure 5
   python3 print_opcode_histogram.py
   # For figure 4(b)
   python3 print_lds_bank_conflict.py
   # For figure 4(c)
   python3 print_overhead.py
   ```