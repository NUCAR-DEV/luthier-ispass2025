FROM ubuntu:24.04

LABEL maintainer=raayaiardakani.m@northeastern.edu

# Build Arguments

# Version of AMD ROCm used
ARG ROCM_VERSION=6.2.4
# Version of neovim used
ARG NVIM_VER=0.10.2
# Place to clone the source code of LLVM project used for building the Luthier project
ARG LUTHIER_LLVM_SRC_DIR=/src/
# Prefix for where Luthier compiler dependencies will be installed to
ARG LUTHIER_DEP_DIR=/opt/luthier
# CMake build type of the LLVM project used for building Luthier
ARG LUTHIER_LLVM_BUILD_TYPE=RelWithDebInfo
# Whether to build the LLVM project used for building Luthier with shared libraries or static libraries
ARG LUTHIER_LLVM_BUILD_SHARED_LIBS=ON
# Whether to build the LLVM project used for building Luthier with expensive checks or not
ARG LUTHIER_LLVM_ENABLE_EXPENSIVE_CHECKS=OFF
# Whether to enable assertions in the LLVM project used for building Luthier
ARG LLVM_ENABLE_ASSERTIONS=OFF
# The git commit hash for the LLVM project used to build Luthier
ARG LUTHIER_LLVM_GIT_HASH=5e51f7702e2703df95f7a3d57284a1fdef4766b7
# The git commit hash for the Luthier project
ARG LUTHIER_GIT_HASH=fdcea25b9a7cd075cfc14ba4f6681fff7a60a0e8
# The git commit hash for HeCBench
ARG HECBENCH_GIT_HASH=b59cdcc3755c3a0cd39b4b9925ac5aa76b1d1171
# Intel LLVM SYCL installation directory
ARG LLVM_SYCL_INSTALL_DIR=/opt/sycl/llvm
# The git commit for the Intel Sycl used to build HeCBench entries
ARG SYCL_GIT_HASH=736534405e9d512c0e2d39f4b6f32573bead30b1
# Where Luthier and HecBench will be cloned and built + All other support files will be put in
ARG WORK_DIR=/work

# Install dependencies
RUN apt-get clean && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y wget ca-certificates curl  \
    build-essential  software-properties-common cmake g++-12 libstdc++-12-dev rpm libelf-dev libnuma-dev sudo  \
    libdw-dev git python3 python3-pip gnupg unzip ripgrep libelf1 file pkg-config xxd ninja-build zsh git npm  \
    python3.12-venv nodejs kmod
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 20 --slave /usr/bin/g++ g++ /usr/bin/g++-12

# ROCm installation
RUN echo "Package: *\nPin: origin ""\nPin-Priority: 600" > /etc/apt/preferences.d/rocm-pin-600
RUN curl -sL https://repo.radeon.com/rocm/rocm.gpg.key | apt-key add - \
  && printf "deb [arch=amd64] https://repo.radeon.com/rocm/apt/$ROCM_VERSION/ noble main" |  \
    tee --append /etc/apt/sources.list.d/rocm.list \
  && printf "deb [arch=amd64] https://repo.radeon.com/amdgpu/$ROCM_VERSION/ubuntu noble main" |  \
    tee /etc/apt/sources.list.d/amdgpu.list \
  && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends  \
    rocm-libs${ROCM_VERSION} \
    rocm-dev${ROCM_VERSION} \
    rocm-llvm-dev${ROCM_VERSION} && apt-get clean && rm -rf /var/lib/apt/lists/*

# Install neovim from AppImage
RUN wget https://github.com/neovim/neovim/releases/download/v${NVIM_VER}/nvim.appimage && \
        chmod +x ./nvim.appimage && ./nvim.appimage --appimage-extract && rm nvim.appimage && \
        cp -r ./squashfs-root/* / && rm -rf ./squashfs-root/

# Install Python packages for Luthier
RUN pip3 install cxxheaderparser pcpp --break-system-packages

# Clone all LLVM-based ROCm projects in the $LUTHIER_LLVM_SRC_DIR directory
RUN mkdir $LUTHIER_LLVM_SRC_DIR && cd $LUTHIER_LLVM_SRC_DIR &&  \
    wget https://github.com/ROCm/llvm-project/archive/${LUTHIER_LLVM_GIT_HASH}.zip  \
    && unzip ${LUTHIER_LLVM_GIT_HASH}.zip && rm ${LUTHIER_LLVM_GIT_HASH}.zip && \
    mv llvm-project-${LUTHIER_LLVM_GIT_HASH}/ llvm-project/
RUN mkdir $LUTHIER_LLVM_SRC_DIR/llvm-project/build && cd $LUTHIER_LLVM_SRC_DIR/llvm-project/build &&  \
    cmake -G Ninja -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=$LUTHIER_LLVM_BUILD_SHARED_LIBS \
    -DCMAKE_INSTALL_PREFIX=${LUTHIER_DEP_DIR}/llvm/  \
    -DCMAKE_BUILD_TYPE=$LUTHIER_LLVM_BUILD_TYPE \
    -DLLVM_TARGETS_TO_BUILD="AMDGPU;X86"  \
    -DLLVM_ENABLE_PROJECTS="llvm;clang;lld;compiler-rt;clang-tools-extra" \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
    -DLLVM_ENABLE_EXPENSIVE_CHECKS=$LUTHIER_LLVM_ENABLE_EXPENSIVE_CHECKS \
    -DLIBCXX_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXX_INSTALL_LIBRARY=OFF \
    -DLIBCXX_INSTALL_HEADERS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXXABI_ENABLE_STATIC=ON \
    -DLIBCXXABI_INSTALL_STATIC_LIBRARY=OFF \
    -DLLVM_ENABLE_RTTI=ON \
    -DLLVM_OPTIMIZED_TABLEGEN=ON \
    -DCLANG_ENABLE_AMDCLANG=ON \
    -DLLVM_BUILD_TOOLS=ON \
    -DLLVM_BUILD_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_BUILD_TESTS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF \
    -DLLVM_BUILD_DOCS=OFF \
    -DLLVM_ENABLE_SPHINX=OFF \
    -DSPHINX_WARNINGS_AS_ERRORS=OFF \
    -DSPHINX_OUTPUT_MAN=OFF \
    -DLLVM_ENABLE_ASSERTIONS=$LLVM_ENABLE_ASSERTIONS \
    -DLLVM_ENABLE_Z3_SOLVER=OFF \
    -DLLVM_ENABLE_ZLIB=ON \
    -DLLVM_AMDGPU_ALLOW_NPI_TARGETS=ON \
    -DCLANG_DEFAULT_PIE_ON_LINUX=0 \
    -DCLANG_DEFAULT_LINKER=lld \
    -DCLANG_DEFAULT_RTLIB=compiler-rt \
    -DCLANG_DEFAULT_UNWINDLIB=libgcc \
    -DSANITIZER_AMDGPU=OFF \
    -DPACKAGE_VENDOR="AMD" \
    -DCLANG_LINK_FLANG_LEGACY=ON \
    -DCMAKE_SKIP_BUILD_RPATH=TRUE \
    -DCMAKE_SKIP_INSTALL_RPATH=TRUE \
    -DFLANG_INCLUDE_DOCS=OFF \
    ../llvm && LD_LIBRARY_PATH=$LUTHIER_LLVM_SRC_DIR/llvm-project/build/lib ninja install &&  \
    rm -rf $LUTHIER_LLVM_SRC_DIR/llvm-project/build

## Install ROCm Device Libs for Luthier
RUN cd $LUTHIER_LLVM_SRC_DIR/llvm-project/amd/device-libs && mkdir build && cd build &&  \
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release  \
    -DCMAKE_INSTALL_PREFIX=${LUTHIER_DEP_DIR}  \
    -DCMAKE_PREFIX_PATH="${LUTHIER_DEP_DIR}" ../ && LD_LIBRARY_PATH=${LUTHIER_DEP_DIR}/llvm/lib/ ninja install &&  \
    rm -rf $LUTHIER_LLVM_SRC_DIR/llvm-project/amd/device-libs/build/

## Install HIPCC for Luthier
RUN cd $LUTHIER_LLVM_SRC_DIR/llvm-project/amd/hipcc/ && mkdir build && cd build &&  \
    cmake -G Ninja -DCMAKE_INSTALL_PREFIX=${LUTHIER_DEP_DIR} -DCMAKE_PREFIX_PATH="${LUTHIER_DEP_DIR}"  \
    -DCMAKE_BUILD_TYPE=Release  \
    .. && LD_LIBRARY_PATH=${LUTHIER_DEP_DIR}/llvm/lib/ ninja install && \
    rm -rf $LUTHIER_LLVM_SRC_DIR/llvm-project/amd/hipcc/build/

## Install SYCL
RUN cd  && wget https://github.com/intel/llvm/archive/${SYCL_GIT_HASH}.zip && unzip ${SYCL_GIT_HASH}.zip && \
     rm ${SYCL_GIT_HASH}.zip && mv llvm-${SYCL_GIT_HASH}/ llvm/ && cd llvm && \
     python3 ./buildbot/configure.py --hip -t release --cmake-gen "Ninja" && cd build \
     && cmake -DCMAKE_INSTALL_PREFIX=${LLVM_SYCL_INSTALL_DIR} ../llvm/ && ninja install && rm -rf llvm/

## Set the LD_LIBRARY_PATH environment variable
ENV LD_LIBRARY_PATH="/opt/rocm/lib:${LUTHIER_DEP_DIR}/llvm/lib/:${LUTHIER_DEP_DIR}/lib"

## Clone the Luthier project and build it
RUN mkdir ${WORK_DIR} && cd ${WORK_DIR} &&  \
    wget https://github.com/matinraayai/Luthier/archive/${LUTHIER_GIT_HASH}.zip && \
    unzip ${LUTHIER_GIT_HASH}.zip && rm ${LUTHIER_GIT_HASH}.zip && mv Luthier-${LUTHIER_GIT_HASH} Luthier/ && \
    mkdir Luthier/build && cd Luthier/build &&  \
    cmake -DCMAKE_PREFIX_PATH="/opt/luthier;/opt/rocm" -G Ninja  \
    -DCMAKE_HIP_COMPILER=/opt/luthier/llvm/bin/clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DLUTHIER_LUTHIER_LLVM_SRC_DIR=${LUTHIER_LLVM_SRC_DIR}/llvm-project \
    -DLLVM_DIR=/opt/luthier/llvm/lib/cmake/llvm/ -DCMAKE_HIP_FLAGS="-O3" ..

## Clone HecBench
RUN cd ${WORK_DIR} && wget https://github.com/zjin-lcf/HeCBench/archive/${HECBENCH_GIT_HASH}.zip && \
    unzip ${HECBENCH_GIT_HASH}.zip && rm ${HECBENCH_GIT_HASH}.zip

WORKDIR ${WORK_DIR}