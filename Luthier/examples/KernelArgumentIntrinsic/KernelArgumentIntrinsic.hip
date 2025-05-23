//===-- KernelArgumentIntrinsic.hip --------------------------------------===//
// Copyright 2022-2025 @ Northeastern University Computer Architecture Lab
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===----------------------------------------------------------------------===//
///
/// \file
/// This file showcases how to use LLVM/Luthier intrinsics inside Luthier
/// hooks.
//===----------------------------------------------------------------------===//
#include <luthier/luthier.h>
#include <mutex>
#include <thread>

#include <hip/hip_runtime_api.h>
#include <llvm/Demangle/Demangle.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "luthier-kernarg-instrinsic"

using namespace luthier;

//===----------------------------------------------------------------------===//
// Commandline arguments for the tool
//===----------------------------------------------------------------------===//

static llvm::cl::OptionCategory *KernelArgumentIntrinsicToolOptionCategory;

static llvm::cl::opt<unsigned int> *InstrBeginInterval;

static llvm::cl::opt<unsigned int> *InstrEndInterval;

static llvm::cl::opt<unsigned int> *KernelBeginInterval;

static llvm::cl::opt<unsigned int> *KernelEndInterval;

static llvm::cl::opt<bool> *DemangleKernelNames;

//===----------------------------------------------------------------------===//
// Global variables of the tool
//===----------------------------------------------------------------------===//

static std::string *ToolName;

/// Number of kernels launched so far
static uint32_t NumKernelLaunched = 0;

/// Kernel instruction counter
__attribute__((managed)) dim3 BlockCount{0, 0, 0};

/// Keeps track of whether we are in the interval to profile or not
static bool ActiveRegion = true;

/// A Mutex, used to protect the Counter value and ActiveRegion
std::mutex Mutex;

static const luthier::hsa::LoadedCodeObjectKernel *KernelBeingInstrumented{
    nullptr};

static bool MustDestroySignalAfterLaunch{false};

static hsa_signal_value_t SignalValue{0};

//===----------------------------------------------------------------------===//
// Hook definitions
//===----------------------------------------------------------------------===//

/// If the Luthier tool contains a hook, then this macro must be used to mark it
/// for Luthier's Tool Executable Manager
MARK_LUTHIER_DEVICE_MODULE

LUTHIER_HOOK_ANNOTATE accessKernelArg() { BlockCount = gridDim; }

LUTHIER_EXPORT_HOOK_HANDLE(accessKernelArg);

//===----------------------------------------------------------------------===//
// Tool Callbacks
//===----------------------------------------------------------------------===//

static llvm::Error instrumentationLoop(InstrumentationTask &IT,
                                       LiftedRepresentation &LR) {
  return LR.iterateAllDefinedFunctionTypes(
      [&](const hsa::LoadedCodeObjectSymbol &Sym,
          llvm::MachineFunction &MF) -> llvm::Error {
        LUTHIER_RETURN_ON_ERROR(IT.insertHookBefore(
            MF.begin()->front(), LUTHIER_GET_HOOK_HANDLE(accessKernelArg)));
        return llvm::Error::success();
      });
}

static void
instrumentAllFunctionsOfLR(const hsa::LoadedCodeObjectKernel &Kernel) {
  auto LR = lift(Kernel);
  LUTHIER_REPORT_FATAL_ON_ERROR(LR.takeError());

  LUTHIER_REPORT_FATAL_ON_ERROR(
      instrumentAndLoad(Kernel, *LR, instrumentationLoop, "kernarg_access"));
}

static void atHsaEvt(hsa::ApiEvtArgs *CBData, ApiEvtPhase Phase,
                     hsa::ApiEvtID ApiID) {
  if (ApiID == luthier::hsa::HSA_API_EVT_ID_hsa_queue_packet_submit) {
    // Copy the app packets because we are going to modify the packets field
    // of the argument, and it will be easier to mutate
    std::vector<hsa::AqlPacket> AppPackets(
        *CBData->hsa_queue_packet_submit.packets);
    auto *ToBeSubmittedPackets = CBData->hsa_queue_packet_submit.packets;
    // Clear the packets that are about to be submitted, as we will repopulate
    // it again after analyzing the AppPackets
    ToBeSubmittedPackets->clear();
    for (auto &Packet : AppPackets) {
      if (auto *DispatchPacket = Packet.asKernelDispatch()) {
        if (Phase == luthier::API_EVT_PHASE_BEFORE) {
          Mutex.lock();
          /// If we are entering to a kernel launch:
          /// 1. Lock the mutex to prevent multiple kernels to run concurrently
          /// 2. Instrument the kernel if no already instrumented
          /// 3. Select whether the instrumented kernel will run or not
          /// 4. Reset the managed Counter variable

          // Get the kernel that is about to get launched
          auto KernelSymbol = hsa::KernelDescriptor::fromKernelObject(
                                  DispatchPacket->kernel_object)
                                  ->getLoadedCodeObjectKernelSymbol();

          LUTHIER_REPORT_FATAL_ON_ERROR(KernelSymbol.takeError());

          KernelBeingInstrumented = KernelSymbol->release();

          auto KernelName = KernelBeingInstrumented->getName();
          LUTHIER_REPORT_FATAL_ON_ERROR(KernelName.takeError());
          std::string KernelNameToBePrinted = DemangleKernelNames
                                                  ? llvm::demangle(*KernelName)
                                                  : std::string(*KernelName);
          llvm::outs() << "Kernel " << KernelNameToBePrinted
                       << " is being launched\n";
          // Get the packet's completion signal and its value

          // Create a signal if the application didn't create one for this
          // packet
          if (DispatchPacket->completion_signal.handle == 0) {
            if (hsa::getHsaApiTable().amd_ext_->hsa_amd_signal_create_fn(
                    SignalValue, 0, nullptr, 0,
                    &DispatchPacket->completion_signal) != HSA_STATUS_SUCCESS)
              llvm::report_fatal_error("Failed to create a signal for the "
                                       "dispatch packet.");
            MustDestroySignalAfterLaunch = true;
          } else {
            MustDestroySignalAfterLaunch = false;
          }
          SignalValue =
              hsa::getHsaApiTable().core_->hsa_signal_load_scacquire_fn(
                  DispatchPacket->completion_signal);

          if (NumKernelLaunched >= *KernelBeginInterval &&
              NumKernelLaunched < *KernelEndInterval) {
            ActiveRegion = true;
          } else {
            ActiveRegion = false;
          }
          llvm::outs() << "Active region: " << ActiveRegion << "\n";

          if (ActiveRegion) {
            llvm::outs() << "Kernel is being instrumented.\n";

            auto IsKernelInstrumented = isKernelInstrumented(
                *KernelBeingInstrumented, "kernarg_access");
            LUTHIER_REPORT_FATAL_ON_ERROR(IsKernelInstrumented.takeError());
            if (!*IsKernelInstrumented) {

              instrumentAllFunctionsOfLR(*KernelBeingInstrumented);
            }
            LUTHIER_REPORT_FATAL_ON_ERROR(luthier::overrideWithInstrumented(
                *DispatchPacket, "kernarg_access"));
            // Zero the counter
            BlockCount = {0, 0, 0};
          }
        } else {
          llvm::outs() << "Active region in the exit callback: " << ActiveRegion
                       << "\n";
          constexpr auto Timeout =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::seconds{300})
                  .count();
          auto CurrentSignalValue =
              hsa::getHsaApiTable().core_->hsa_signal_wait_relaxed_fn(
                  DispatchPacket->completion_signal, HSA_SIGNAL_CONDITION_LT,
                  SignalValue, Timeout, HSA_WAIT_STATE_ACTIVE);
          if (CurrentSignalValue >= SignalValue)
            llvm::report_fatal_error(
                llvm::formatv("Wait timeout reached for signal handle {0:x},"
                              "Expected "
                              "value: {1}, Actual Value: {2}",
                              DispatchPacket->completion_signal.handle,
                              CurrentSignalValue - 1, SignalValue));
          if (MustDestroySignalAfterLaunch) {
            if (hsa::getHsaApiTable().core_->hsa_signal_destroy_fn(
                    DispatchPacket->completion_signal) != HSA_STATUS_SUCCESS) {
              llvm::report_fatal_error(
                  "Failed to destroy the completed kernel's"
                  "signal");
            }
          }
          if (ActiveRegion) {
            llvm::outs() << "Kernel is being waited on\n";
            /// If we are exiting a kernel launch:
            /// 1. Wait on the signal of the kernel being launched
            /// 2. Get the number of wave fronts in the kernel
            /// 3. Print the counter for this kernel
            /// 4. Release the lock

            // Get the kernel that was launched

            auto KernelName = KernelBeingInstrumented->getName();
            LUTHIER_REPORT_FATAL_ON_ERROR(KernelName.takeError());
            std::string KernelNameToBePrinted =
                DemangleKernelNames ? llvm::demangle(*KernelName)
                                    : std::string(*KernelName);

            // Note: Grid size in HSA contains all the work items in the kernel.
            // This is different from the number of blocks (grid dim) in CUDA or
            // HIP For example, if a kernel is launched from HIP with 16 blocks,
            // each with 64 threads, this will be indicated as a grid size dim
            // of (1024, 1, 1) and workgroup size of (64, 1, 1), since 1024 = 64
            // x 16. As is evident from the example, workgroup size in HSA has
            // the same meaning as CUDA/HIP
            llvm::outs() << llvm::formatv(
                "Kernel {0} - {1}: Total Grid Size Dims: ({2}, {3}, {4}) "
                "Workgroup Dims: "
                "({5}, {6}, {7}) Kernel block count: ({8}, {9}, {10}).\n",
                NumKernelLaunched, KernelNameToBePrinted,
                DispatchPacket->grid_size_x, DispatchPacket->grid_size_y,
                DispatchPacket->grid_size_z, DispatchPacket->workgroup_size_x,
                DispatchPacket->workgroup_size_y,
                DispatchPacket->workgroup_size_z, BlockCount.x, BlockCount.y,
                BlockCount.z);
          }
          NumKernelLaunched++;
          Mutex.unlock();
        }
      }
      if (Phase == API_EVT_PHASE_BEFORE)
        ToBeSubmittedPackets->push_back(Packet);
    }
  }
}
namespace luthier {

llvm::StringRef getToolName() { return *ToolName; }

static void atHsaApiTableCaptureCallBack(ApiEvtPhase Phase) {
  if (Phase == API_EVT_PHASE_BEFORE) {
    KernelArgumentIntrinsicToolOptionCategory =
        new llvm::cl::OptionCategory("Kernel Argument Intrinsic Tool Options");

    InstrBeginInterval = new llvm::cl::opt<unsigned int>(
        "instr-begin-interval",
        llvm::cl::desc("Beginning of the instruction interval where to "
                       "apply instrumentation, inclusive"),
        llvm::cl::init(0), llvm::cl::NotHidden,
        llvm::cl::cat(*KernelArgumentIntrinsicToolOptionCategory));

    InstrEndInterval = new llvm::cl::opt<unsigned int>(
        "instr-end-interval",
        llvm::cl::desc("End of the instruction interval where to "
                       "apply instrumentation, exclusive"),
        llvm::cl::init(std::numeric_limits<unsigned int>::max()),
        llvm::cl::NotHidden,
        llvm::cl::cat(*KernelArgumentIntrinsicToolOptionCategory));

    KernelBeginInterval = new llvm::cl::opt<unsigned int>(
        "kernel-begin-interval",
        llvm::cl::desc("Beginning of the kernel interval to apply "
                       "instrumentation, inclusive"),
        llvm::cl::init(0), llvm::cl::NotHidden,
        llvm::cl::cat(*KernelArgumentIntrinsicToolOptionCategory));

    KernelEndInterval = new llvm::cl::opt<unsigned int>(
        "kernel-end-interval",
        llvm::cl::desc(
            "End of the kernel interval to apply instrumentation, exclusive"),
        llvm::cl::init(std::numeric_limits<unsigned int>::max()),
        llvm::cl::NotHidden,
        llvm::cl::cat(*KernelArgumentIntrinsicToolOptionCategory));

    DemangleKernelNames = new llvm::cl::opt<bool>(
        "demangle-kernel-names",
        llvm::cl::desc("Whether to demangle kernel names before printing"),
        llvm::cl::init(true), llvm::cl::NotHidden,
        llvm::cl::cat(*KernelArgumentIntrinsicToolOptionCategory));

    ToolName = new std::string{"LuthierKernelArgumentIntrinsicTool"};
  }

  if (Phase == API_EVT_PHASE_AFTER) {
    LUTHIER_REPORT_FATAL_ON_ERROR(hsa::enableHsaApiEvtIDCallback(
        hsa::HSA_API_EVT_ID_hsa_queue_packet_submit));
  }
}

void atToolInit(ApiEvtPhase Phase) {
  if (Phase == API_EVT_PHASE_BEFORE) {
    llvm::outs() << "Instruction counter tool is launching.\n";
  } else {
    // Set the callback for when the HSA API table is captured
    hsa::setAtApiTableCaptureEvtCallback(atHsaApiTableCaptureCallBack);
    // Set the HSA API callback
    hsa::setAtHsaApiEvtCallback(atHsaEvt);
  }
}

void atToolFini(ApiEvtPhase Phase) {

  delete InstrBeginInterval;

  delete InstrEndInterval;

  delete KernelBeginInterval;

  delete KernelEndInterval;

  delete DemangleKernelNames;

  delete ToolName;

  delete KernelArgumentIntrinsicToolOptionCategory;

}

} // namespace luthier
