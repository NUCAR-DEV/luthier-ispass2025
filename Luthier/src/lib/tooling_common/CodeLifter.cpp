//===-- CodeLifter.cpp - Luthier's Code Lifter  ---------------------------===//
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
/// This file implements Luthier's Code Lifter.
//===----------------------------------------------------------------------===//
#include "tooling_common/CodeLifter.hpp"

#include "LuthierRealToPseudoOpcodeMap.hpp"
#include "LuthierRealToPseudoRegEnumMap.hpp"

#include "common/ObjectUtils.hpp"
#include "hsa/Executable.hpp"
#include "hsa/GpuAgent.hpp"
#include "hsa/ISA.hpp"
#include "hsa/LoadedCodeObject.hpp"
#include "hsa/hsa.hpp"
#include "luthier/hsa/Instr.h"
#include "luthier/hsa/KernelDescriptor.h"
#include "luthier/llvm/streams.h"
#include "luthier/tooling/LRCallgraph.h"
#include "luthier/types.h"
#include "tooling_common/TargetManager.hpp"
#include <GCNSubtarget.h>
#include <SIInstrInfo.h>
#include <SIMachineFunctionInfo.h>
#include <SIRegisterInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/BinaryFormat/MsgPackDocument.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/CodeGen/LivePhysRegs.h>
#include <llvm/CodeGen/MachineFrameInfo.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCFixupKindInfo.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrAnalysis.h>
#include <llvm/MC/MCInstrDesc.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSymbolELF.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/RelocationResolver.h>
#include <llvm/Support/AMDGPUAddrSpace.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>
#include <luthier/hsa/LoadedCodeObjectExternSymbol.h>
#include <luthier/hsa/LoadedCodeObjectVariable.h>
#include <memory>

#undef DEBUG_TYPE
#define DEBUG_TYPE "luthier-code-lifter"

namespace luthier {

template <> CodeLifter *Singleton<CodeLifter>::Instance{nullptr};

llvm::Error CodeLifter::invalidateCachedExecutableItems(hsa::Executable &Exec) {
    // TODO: Re-enable this once the executable cache is implemented
    //  std::lock_guard Lock(CacheMutex);
    //  llvm::SmallVector<hsa::LoadedCodeObject, 1> LCOs;
    //  LUTHIER_RETURN_ON_ERROR(Exec.getLoadedCodeObjects(LCOs));
    //
    //  for (const auto &LCO : LCOs) {
    //     Remove the branch and branch target locations
    //    if (DirectBranchTargetLocations.contains(LCO))
    //      DirectBranchTargetLocations.erase(LCO);
    //     Remove relocation info
    //    if (Relocations.contains(LCO)) {
    //      Relocations.erase(LCO);
    //    }
    //
    //    llvm::SmallVector<std::unique_ptr<hsa::LoadedCodeObjectSymbol>> Symbols;
    //    LUTHIER_RETURN_ON_ERROR(LCO.getKernelSymbols(Symbols));
    //    LUTHIER_RETURN_ON_ERROR(LCO.getDeviceFunctionSymbols(Symbols));
    //    for (const auto &Symbol : Symbols) {
    //       Remove the disassembled hsa::Instr of each hsa::ExecutableSymbol
    //      if (MCDisassembledSymbols.contains(Symbol))
    //        MCDisassembledSymbols.erase(Symbol);
    //       Remove its lifted representation if this is a kernel
    //      if (auto *KernelSymbol =
    //              llvm::dyn_cast<hsa::LoadedCodeObjectKernel>(Symbol.get())) {
    //        if (LiftedKernelSymbols.contains(KernelSymbol))
    //          LiftedKernelSymbols.erase(LiftedKernelSymbols.find(KernelSymbol));
    //      }
    //    }
    //  }
    return llvm::Error::success();
}

bool CodeLifter::evaluateBranch(const llvm::MCInst &Inst, uint64_t Addr,
                                uint64_t Size, uint64_t &Target) {
    if (!Inst.getOperand(0).isImm())
        return false;
    int64_t Imm = Inst.getOperand(0).getImm();
    // Our branches take a simm16.
    Target = llvm::SignExtend64<16>(Imm) * 4 + Addr + 4;
    return true;
}

llvm::Expected<CodeLifter::DisassemblyInfo &>
luthier::CodeLifter::getDisassemblyInfo(const hsa::ISA &ISA) {
    if (!DisassemblyInfoMap.contains(ISA)) {
        auto TargetInfo = TargetManager::instance().getTargetInfo(ISA);
        LUTHIER_RETURN_ON_ERROR(TargetInfo.takeError());

        auto TT = ISA.getTargetTriple();
        LUTHIER_RETURN_ON_ERROR(TT.takeError());

        std::unique_ptr<llvm::MCContext> MCCtx(new (std::nothrow) llvm::MCContext(
            llvm::Triple(*TT), TargetInfo->getMCAsmInfo(),
            TargetInfo->getMCRegisterInfo(), TargetInfo->getMCSubTargetInfo()));
        LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
            MCCtx != nullptr,
            "Failed to create MCContext for LLVM disassembly operation."));

        std::unique_ptr<llvm::MCDisassembler> DisAsm(
            TargetInfo->getTarget()->createMCDisassembler(
                *(TargetInfo->getMCSubTargetInfo()), *MCCtx));
        LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
            DisAsm != nullptr, "Failed to create an MCDisassembler for the LLVM "
                               "disassembly operation."));

        DisassemblyInfoMap.insert(
            {ISA, DisassemblyInfo{std::move(MCCtx), std::move(DisAsm)}});
    }
    return DisassemblyInfoMap[ISA];
}

bool CodeLifter::isAddressDirectBranchTarget(const hsa::LoadedCodeObject &LCO,
                                             address_t Address) {
    if (!DirectBranchTargetLocations.contains(LCO)) {
        return false;
    }
    auto &AddressInfo = DirectBranchTargetLocations[LCO];
    return AddressInfo.contains(Address);
}

void luthier::CodeLifter::addDirectBranchTargetAddress(
    const hsa::LoadedCodeObject &LCO, address_t Address) {
    if (!DirectBranchTargetLocations.contains(LCO)) {
        DirectBranchTargetLocations.insert(
            {LCO, llvm::DenseSet<luthier::address_t>{}});
    }
    DirectBranchTargetLocations[LCO].insert(Address);
}

llvm::Expected<
    std::pair<std::vector<llvm::MCInst>, std::vector<luthier::address_t>>>
CodeLifter::disassemble(const hsa::ISA &ISA, llvm::ArrayRef<uint8_t> Code) {
    auto DisassemblyInfo = getDisassemblyInfo(ISA);
    LUTHIER_RETURN_ON_ERROR(DisassemblyInfo.takeError());

    auto TargetInfo = TargetManager::instance().getTargetInfo(ISA);
    LUTHIER_RETURN_ON_ERROR(TargetInfo.takeError());
    const auto &DisAsm = DisassemblyInfo->DisAsm;

    size_t MaxReadSize = TargetInfo->getMCAsmInfo()->getMaxInstLength();
    size_t Idx = 0;
    luthier::address_t CurrentAddress = 0;
    std::vector<llvm::MCInst> Instructions;
    std::vector<luthier::address_t> Addresses;

    while (Idx < Code.size()) {
        size_t ReadSize =
            (Idx + MaxReadSize) < Code.size() ? MaxReadSize : Code.size() - Idx;
        size_t InstSize{};
        llvm::MCInst Inst;
        auto ReadBytes =
            arrayRefFromStringRef(toStringRef(Code).substr(Idx, ReadSize));
        LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
            DisAsm->getInstruction(Inst, InstSize, ReadBytes, CurrentAddress,
                                   llvm::nulls()) == llvm::MCDisassembler::Success,
            "Failed to disassemble instruction at address {0:x}", CurrentAddress));

        Addresses.push_back(CurrentAddress);
        Idx += InstSize;
        CurrentAddress += InstSize;
        Instructions.push_back(Inst);
    }

    return std::make_pair(Instructions, Addresses);
}

llvm::Expected<const CodeLifter::LCORelocationInfo *>
CodeLifter::resolveRelocation(const hsa::LoadedCodeObject &LCO,
                              luthier::address_t Address) {
    if (!Relocations.contains(LCO)) {
        // If the LCO doesn't have its relocation info cached, calculate it
        auto LoadedMemory = LCO.getLoadedMemory();
        LUTHIER_RETURN_ON_ERROR(LoadedMemory.takeError());

        auto LoadedMemoryBase = reinterpret_cast<address_t>(LoadedMemory->data());

        llvm::Expected<luthier::AMDGCNObjectFile &> StorageELFOrErr =
            LCO.getStorageELF();
        LUTHIER_RETURN_ON_ERROR(StorageELFOrErr.takeError());

        // Create an entry for the LCO in the relocations map
        auto &LCORelocationsMap =
            Relocations
                .insert({LCO, llvm::DenseMap<address_t, LCORelocationInfo>{}})
                .first->getSecond();

        for (const auto &Section : StorageELFOrErr->sections()) {
            for (const llvm::object::ELFRelocationRef Reloc : Section.relocations()) {
                // Only rely on the loaded address of the symbol instead of its name
                // The name will be stripped from the relocation section
                // if the symbol has a private linkage (i.e. device functions)
                auto RelocSym = Reloc.getSymbol();
                if (RelocSym != StorageELFOrErr->symbol_end()) {
                    auto RelocSymbolLoadedAddress = Reloc.getSymbol()->getAddress();
                    LUTHIER_RETURN_ON_ERROR(RelocSymbolLoadedAddress.takeError());
                    LLVM_DEBUG(
                    LUTHIER_RETURN_ON_MOVE_INTO_FAIL(llvm::StringRef, SymName,
                                                     Reloc.getSymbol()->getName());
                    llvm::dbgs() << llvm::formatv(
                        "Found relocation for symbol {0} at address {1:x}.\n",
                        SymName, LoadedMemoryBase + *RelocSymbolLoadedAddress));
                    // Check with the hsa::Platform which HSA executable Symbol this
                    // address is associated with
                    auto RelocSymbol = hsa::LoadedCodeObjectSymbol::fromLoadedAddress(
                        LoadedMemoryBase + *RelocSymbolLoadedAddress);
                    LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
                        *RelocSymbol != nullptr,
                        "Failed to find a symbol associated with device "
                        "address {0:x}.",
                        LoadedMemoryBase + *RelocSymbolLoadedAddress));
                    // The target address will be the base of the loaded
                    luthier::address_t TargetAddress =
                        LoadedMemoryBase + Reloc.getOffset();
                    LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
                        "Relocation found for symbol {0} at address {1:x} for "
                        "LCO {2:x}.\n",
                        llvm::cantFail(RelocSymbol.get()->getName()),
                        TargetAddress, LCO.hsaHandle()));
                    LCORelocationsMap.insert(
                        {TargetAddress,
                         LCORelocationInfo{std::move(RelocSymbol.get()->clone()),
                                           Reloc}});
                }
            }
        }
    }
    // Querying actually begins here
    const auto &LCORelocationsMap = Relocations.at(LCO);
    LLVM_DEBUG(
        llvm::dbgs() << llvm::formatv("Querying address {0:x} for LCO {1:x}\n",
                                      Address, LCO.hsaHandle()));
    if (LCORelocationsMap.contains(Address)) {
        auto &Out = LCORelocationsMap.at(Address);
        LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
            "Relocation information found for loaded "
            "address: {0:x}, targeting symbol {1}.\n",
            Address, llvm::cantFail(Out.Symbol->getName())));

        return &Out;
    }
    return nullptr;
}

llvm::Error CodeLifter::initLR(LiftedRepresentation &LR,
                               const hsa::LoadedCodeObjectKernel &Kernel) {
    // Create a thread-safe LLVMContext
    LR.Context =
        llvm::orc::ThreadSafeContext(std::make_unique<llvm::LLVMContext>());
    // Get the LCO of the kernel
    hsa::LoadedCodeObject LCO(Kernel.getLoadedCodeObject());
    LR.LCO = LCO.asHsaType();
    // Create a new Target Machine for the LCO
    auto ISA = LCO.getISA();
    LUTHIER_RETURN_ON_ERROR(ISA.takeError());
    LUTHIER_RETURN_ON_ERROR(
        TargetManager::instance().createTargetMachine(*ISA).moveInto(LR.TM));
    // Enable the AsmVerbose option in case we're printing a .s file
    LR.TM->Options.MCOptions.AsmVerbose = true;
    // Create the llvm::Module for this LCO
    llvm::Expected<llvm::StringRef> KernelNameOrErr = Kernel.getName();
    LUTHIER_RETURN_ON_ERROR(KernelNameOrErr.takeError());

    LR.Module = std::make_unique<llvm::Module>(*KernelNameOrErr,
                                               *LR.Context.getContext());
    // Set the data layout
    LR.Module->setDataLayout(LR.TM->createDataLayout());
    // Create the MMIWP which will store the MIR of the LCO
    LR.MMIWP = std::make_unique<llvm::MachineModuleInfoWrapperPass>(LR.TM.get());

    return llvm::Error::success();
}

llvm::Error
CodeLifter::initLiftedGlobalVariableEntry(const hsa::LoadedCodeObject &LCO,
                                          const hsa::LoadedCodeObjectSymbol &GV,
                                          LiftedRepresentation &LR) {
    auto &LLVMContext = LR.getContext();
    auto GVName = GV.getName();
    LUTHIER_RETURN_ON_ERROR(GVName.takeError());
    size_t GVSize = GV.getSize();
    // Lift each variable as an array of bytes, with a length of GVSize
    // We remove any initializers present in the LCO
    LR.Variables[GV.clone()] = new llvm::GlobalVariable(
        *LR.Module,
        llvm::ArrayType::get(llvm::Type::getInt8Ty(LLVMContext), GVSize), false,
        llvm::GlobalValue::LinkageTypes::ExternalLinkage, nullptr, *GVName);
    return llvm::Error::success();
}

static llvm::Type *
processExplicitKernelArg(const hsa::md::Kernel::Arg::Metadata &ArgMD,
                         llvm::LLVMContext &Ctx) {
    llvm::Type *ParamType = llvm::Type::getIntNTy(Ctx, ArgMD.Size * 8);
    // Used when the argument kind is global buffer or dynamic shared pointer
    unsigned int AddressSpace = ArgMD.AddressSpace.has_value()
                                ? *ArgMD.AddressSpace
                                : llvm::AMDGPUAS::GLOBAL_ADDRESS;
    unsigned int PointeeAlign =
        ArgMD.PointeeAlign.has_value() ? *ArgMD.PointeeAlign : 0;
    switch (ArgMD.ValueKind) {
        case hsa::md::ValueKind::ByValue:
            break;
        case hsa::md::ValueKind::GlobalBuffer:
            // Convert the argument to a pointer
            ParamType = llvm::PointerType::get(ParamType, AddressSpace);
            break;
        default:
            llvm_unreachable("Not implemented");
    }
    return ParamType;
}

void processHiddenKernelArg(const hsa::md::Kernel::Arg::Metadata &ArgMD,
                            llvm::Function &F, llvm::SIMachineFunctionInfo &MFI,
                            const llvm::SIRegisterInfo &TRI) {
    switch (ArgMD.ValueKind) {
        case hsa::md::ValueKind::HiddenGlobalOffsetX:
        case hsa::md::ValueKind::HiddenGlobalOffsetY:
        case hsa::md::ValueKind::HiddenGlobalOffsetZ:
        case hsa::md::ValueKind::HiddenBlockCountX:
        case hsa::md::ValueKind::HiddenBlockCountY:
        case hsa::md::ValueKind::HiddenBlockCountZ:
        case hsa::md::ValueKind::HiddenRemainderX:
        case hsa::md::ValueKind::HiddenRemainderY:
        case hsa::md::ValueKind::HiddenRemainderZ:
        case hsa::md::ValueKind::HiddenNone:
        case hsa::md::ValueKind::HiddenGroupSizeX:
        case hsa::md::ValueKind::HiddenGroupSizeY:
        case hsa::md::ValueKind::HiddenGroupSizeZ:
        case hsa::md::ValueKind::HiddenGridDims:
        case hsa::md::ValueKind::HiddenPrivateBase:
        case hsa::md::ValueKind::HiddenSharedBase:
            break;
        case hsa::md::ValueKind::HiddenPrintfBuffer:
            F.getParent()->getOrInsertNamedMetadata("llvm.printf.fmts");
            break;
        case hsa::md::ValueKind::HiddenHostcallBuffer:
            F.removeFnAttr("amdgpu-no-hostcall-ptr");
            break;
        case hsa::md::ValueKind::HiddenDefaultQueue:
            F.removeFnAttr("amdgpu-no-default-queue");
            break;
        case hsa::md::ValueKind::HiddenCompletionAction:
            F.removeFnAttr("amdgpu-no-completion-action");
            break;
        case hsa::md::ValueKind::HiddenMultiGridSyncArg:
            F.removeFnAttr("amdgpu-no-multigrid-sync-arg");
            break;
        case hsa::md::ValueKind::HiddenHeapV1:
            F.removeFnAttr("amdgpu-no-heap-ptr");
            break;
        case hsa::md::ValueKind::HiddenDynamicLDSSize:
            MFI.setUsesDynamicLDS(true);
            break;
        case hsa::md::ValueKind::HiddenQueuePtr:
            MFI.addQueuePtr(TRI);
            break;
        default:
            return;
    }
}

llvm::Error
CodeLifter::initLiftedKernelEntry(const hsa::LoadedCodeObjectKernel &Kernel,
                                  LiftedRepresentation &LR) {
    llvm::LLVMContext &LLVMContext = *LR.Context.getContext();
    llvm::Module &Module = *LR.Module;
    llvm::MachineModuleInfo &MMI = LR.MMIWP->getMMI();
    // Populate the Arguments ==================================================
    auto SymbolName = Kernel.getName();
    LUTHIER_RETURN_ON_ERROR(SymbolName.takeError());
    const auto &KernelMD = Kernel.getKernelMetadata();

    // Kernel's return type is always void
    llvm::Type *const ReturnType = llvm::Type::getVoidTy(LLVMContext);

    // Create the Kernel's FunctionType with appropriate kernel Arguments
    // (if any)
    llvm::SmallVector<llvm::Type *> Params;
    unsigned int ExplicitArgsOffset = 0;
    unsigned int ImplicitArgsOffset = 0;
    if (KernelMD.Args.has_value()) {
        // Reserve the number of arguments in the Params vector
        Params.reserve(KernelMD.Args->size());
        // For now, we only rely on required argument metadata
        // This should be updated as new cases are encountered
        for (const auto &ArgMD : *KernelMD.Args) {
            if (ArgMD.ValueKind >= hsa::md::ValueKind::HiddenArgKindBegin)
                break;
            else {
                Params.push_back(processExplicitKernelArg(ArgMD, Module.getContext()));
                if (ArgMD.Offset > ExplicitArgsOffset)
                    ExplicitArgsOffset = ArgMD.Offset;
            }
        }
    }

    llvm::FunctionType *FunctionType =
        llvm::FunctionType::get(ReturnType, Params, false);

    auto *F = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::WeakAnyLinkage,
        SymbolName->substr(0, SymbolName->rfind(".kd")), Module);
    F->setVisibility(llvm::GlobalValue::ProtectedVisibility);

    // Populate the Attributes =================================================

    F->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);

    F->addFnAttr("uniform-work-group-size",
                 KernelMD.UniformWorkgroupSize ? "true" : "false");

    // Construct the attributes of the Function, which will result in the MF
    // attributes getting populated
    auto KDOnDevice = Kernel.getKernelDescriptor();
    LUTHIER_RETURN_ON_ERROR(KDOnDevice.takeError());

    auto KDOnHost = hsa::queryHostAddress(*KDOnDevice);
    LUTHIER_RETURN_ON_ERROR(KDOnHost.takeError());

    F->addFnAttr("amdgpu-lds-size",
                 llvm::to_string((*KDOnHost)->GroupSegmentFixedSize));
    // Kern Arg is determined via analysis usage + args set earlier
    auto Rsrc1 = (*KDOnHost)->getRsrc1();
    auto Rsrc2 = (*KDOnHost)->getRsrc2();
    auto KCP = (*KDOnHost)->getKernelCodeProperties();
    if (KCP.EnableSgprDispatchId == 0) {
        F->addFnAttr("amdgpu-no-dispatch-id");
    }
    if (KCP.EnableSgprDispatchPtr == 0) {
        F->addFnAttr("amdgpu-no-dispatch-ptr");
    }
    if (KCP.EnableSgprQueuePtr == 0) {
        F->addFnAttr("amdgpu-no-queue-ptr");
    }

    F->addFnAttr("amdgpu-ieee", Rsrc1.EnableIeeeMode ? "true" : "false");
    F->addFnAttr("amdgpu-dx10-clamp", Rsrc1.EnableDx10Clamp ? "true" : "false");
    if (Rsrc2.EnableSgprWorkgroupIdX == 0) {
        F->addFnAttr("amdgpu-no-workgroup-id-x");
    }
    if (Rsrc2.EnableSgprWorkgroupIdY == 0) {
        F->addFnAttr("amdgpu-no-workgroup-id-y");
    }
    if (Rsrc2.EnableSgprWorkgroupIdZ == 0) {
        F->addFnAttr("amdgpu-no-workgroup-id-z");
    }
    switch (Rsrc2.EnableVgprWorkitemId) {
        case 0:
            F->addFnAttr("amdgpu-no-workitem-id-y");
        case 1:
            F->addFnAttr("amdgpu-no-workitem-id-z");
        case 2:
            break;
        default:
            llvm_unreachable("KD's VGPR workitem ID is not valid");
    }

    // TODO: Check the args metadata to set this correctly
    // TODO: Set the rest of the attributes
    //    luthier::outs() << "Preloaded Args: " << (*KDOnHost)->KernArgPreload <<
    //    "\n";
    //  F->addFnAttr("amdgpu-calls");
    // Add dummy IR instructions ===============================================
    // Very important to have a dummy IR BasicBlock; Otherwise MachinePasses
    // won't run
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(Module.getContext(), "", F);
    new llvm::UnreachableInst(Module.getContext(), BB);

    // Populate the MFI ========================================================

    auto &MF = MMI.getOrCreateMachineFunction(*F);

    // TODO: Fix alignment value depending on the function type
    MF.setAlignment(llvm::Align(4096));
    auto &TM = MMI.getTarget();

    auto TRI = reinterpret_cast<const llvm::SIRegisterInfo *>(
        TM.getSubtargetImpl(*F)->getRegisterInfo());
    auto MFI = MF.template getInfo<llvm::SIMachineFunctionInfo>();
    if (KCP.EnableSgprDispatchPtr == 1) {
        MFI->addDispatchPtr(*TRI);
    }
    if (KCP.EnableSgprPrivateSegmentBuffer == 1) {
        MFI->addPrivateSegmentBuffer(*TRI);
    }
    if (KCP.EnableSgprKernArgSegmentPtr == 1) {
        MFI->addKernargSegmentPtr(*TRI);
    }
    if (KCP.EnableSgprFlatScratchInit == 1) {
        MFI->addFlatScratchInit(*TRI);
    }
    if (Rsrc2.EnableSgprPrivateSegmentWaveByteOffset == 1) {
        MFI->addPrivateSegmentWaveByteOffset();
    }

    // Process the hidden args now that MFI and MF has been created
    if (KernelMD.Args.has_value()) {
        // Add absence of all hidden arguments; As we iterate over all the
        // hidden arguments, we get rid of them if we detect their presence
        F->addFnAttr("amdgpu-no-hostcall-ptr");
        F->addFnAttr("amdgpu-no-default-queue");
        F->addFnAttr("amdgpu-no-completion-action");
        F->addFnAttr("amdgpu-no-multigrid-sync-arg");
        F->addFnAttr("amdgpu-no-heap-ptr");
        for (const auto &ArgMD : *KernelMD.Args) {
            if (ArgMD.ValueKind >= hsa::md::ValueKind::HiddenArgKindBegin &&
                ArgMD.ValueKind <= hsa::md::ValueKind::HiddenArgKindEnd) {
                processHiddenKernelArg(
                    ArgMD, *F, *MFI,
                    *MF.getSubtarget<llvm::GCNSubtarget>().getRegisterInfo());
                if (ArgMD.Offset > ImplicitArgsOffset)
                    ImplicitArgsOffset = ArgMD.Offset;
            }
        }
    }

    // Number of implicit arg bytes is the difference between the last hidden
    // arg offset and the last explicit arg offset
    F->addFnAttr("amdgpu-implicitarg-num-bytes",
                 llvm::to_string(ImplicitArgsOffset - ExplicitArgsOffset));

    LR.Kernel =
        llvm::unique_dyn_cast<hsa::LoadedCodeObjectKernel>(Kernel.clone());
    LR.KernelMF = &MF;
    return llvm::Error::success();
}

llvm::Error CodeLifter::initLiftedDeviceFunctionEntry(
    const hsa::LoadedCodeObjectDeviceFunction &Func, LiftedRepresentation &LR) {
    llvm::LLVMContext &LLVMContext = *LR.Context.getContext();
    llvm::Module &Module = *LR.Module;
    llvm::MachineModuleInfo &MMI = LR.MMIWP->getMMI();

    auto FuncName = Func.getName();
    LUTHIER_RETURN_ON_ERROR(FuncName.takeError());
    llvm::Type *const ReturnType = llvm::Type::getVoidTy(Module.getContext());
    llvm::FunctionType *FunctionType =
        llvm::FunctionType::get(ReturnType, {}, false);

    auto *F = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::PrivateLinkage, *FuncName, Module);
    F->setCallingConv(llvm::CallingConv::C);
    // Add dummy IR instructions ===============================================
    // Very important to have a dummy IR BasicBlock; Otherwise MachinePasses
    // won't run
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(Module.getContext(), "", F);
    LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
        BB != nullptr,
        "Failed to create a dummy IR basic block during code lifting."));
    new llvm::UnreachableInst(Module.getContext(), BB);
    auto &MF = MMI.getOrCreateMachineFunction(*F);

    // TODO: Fix alignment value depending on the function type
    MF.setAlignment(llvm::Align(4096));
    LR.Functions.emplace(
        llvm::unique_dyn_cast<hsa::LoadedCodeObjectDeviceFunction>(Func.clone()),
        &MF);
    return llvm::Error::success();
}

static bool shouldReadExec(const llvm::MachineInstr &MI) {
    if (llvm::SIInstrInfo::isVALU(MI)) {
        switch (MI.getOpcode()) {
            case llvm::AMDGPU::V_READLANE_B32:
            case llvm::AMDGPU::SI_RESTORE_S32_FROM_VGPR:
            case llvm::AMDGPU::V_WRITELANE_B32:
            case llvm::AMDGPU::SI_SPILL_S32_TO_VGPR:
                return false;
        }

        return true;
    }

    if (MI.isPreISelOpcode() ||
        llvm::SIInstrInfo::isGenericOpcode(MI.getOpcode()) ||
        llvm::SIInstrInfo::isSALU(MI) || llvm::SIInstrInfo::isSMRD(MI))
        return false;

    return true;
}

llvm::Error verifyInstruction(llvm::MachineInstrBuilder &Builder) {
    auto &MI = *Builder.getInstr();
    if (shouldReadExec(MI) &&
        !MI.hasRegisterImplicitUseOperand(llvm::AMDGPU::EXEC)) {
        MI.addOperand(
            llvm::MachineOperand::CreateReg(llvm::AMDGPU::EXEC, false, true));
    }
    return llvm::Error::success();
}

static llvm::Error fixupBitsetInst(llvm::MachineInstr &MI) {
    unsigned int Opcode = MI.getOpcode();
    if (Opcode == llvm::AMDGPU::S_BITSET0_B32 ||
        Opcode == llvm::AMDGPU::S_BITSET0_B64 ||
        Opcode == llvm::AMDGPU::S_BITSET1_B32 ||
        Opcode == llvm::AMDGPU::S_BITSET1_B64) {
        // bitset instructions have a tied def/use that is not reflected in the
        // MC version
        if (MI.getNumOperands() < MI.getNumExplicitOperands()) {
            // Check if the first operand is a register
            LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
                MI.getOperand(0).isReg(),
                "The first operand of a bitset instruction is not a register."));
            // Add the output reg also as the first input, and tie the first and
            // second operands together
            MI.addOperand(
                llvm::MachineOperand::CreateReg(MI.getOperand(0).getReg(), false));
            MI.tieOperands(0, 2);
        } else {
            return llvm::Error::success();
        }
    }
    return llvm::Error::success();
}

llvm::Error CodeLifter::liftFunction(const hsa::LoadedCodeObjectSymbol &Symbol,
                                     llvm::MachineFunction &MF,
                                     LiftedRepresentation &LR) {
    hsa::LoadedCodeObject LCO(LR.LCO);
    llvm::Module &Module = LR.getModule();
    llvm::MachineModuleInfo &MMI = LR.getMMI();
    auto &F = MF.getFunction();

    LLVM_DEBUG(llvm::dbgs() << "================================================="
                               "=======================\n";
    llvm::dbgs() << "Populating contents of Machine Function "
                 << MF.getName() << "\n");

    auto &TM = MMI.getTarget();

    auto ISA = LCO.getISA();
    LUTHIER_RETURN_ON_ERROR(ISA.takeError());

    auto TargetInfo = TargetManager::instance().getTargetInfo(*ISA);
    LUTHIER_RETURN_ON_ERROR(TargetInfo.takeError());

    llvm::MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();

    MF.push_back(MBB);
    auto MBBEntry = MBB;

    llvm::MCContext &MCContext = MMI.getContext();

    auto MCInstInfo = TargetInfo->getMCInstrInfo();

    llvm::DenseMap<luthier::address_t,
                   llvm::SmallVector<llvm::MachineInstr *>>
        UnresolvedBranchMIs; // < Set of branch instructions located at a
    // luthier_address_t waiting for their
    // target to be resolved after MBBs and MIs
    // are created
    llvm::DenseMap<luthier::address_t, llvm::MachineBasicBlock *>
        BranchTargetMBBs; // < Set of MBBs that will be the target of the
    // UnresolvedBranchMIs
    auto MIA = TargetInfo->getMCInstrAnalysis();

    llvm::ArrayRef<hsa::Instr> TargetFunction;
    if (const auto *Kernel =
        llvm::dyn_cast<hsa::LoadedCodeObjectKernel>(&Symbol)) {
        LUTHIER_RETURN_ON_ERROR(
            CodeLifter::instance().disassemble(*Kernel).moveInto(TargetFunction));
    } else if (const auto *Func =
        llvm::dyn_cast<hsa::LoadedCodeObjectDeviceFunction>(&Symbol))
        LUTHIER_RETURN_ON_ERROR(
            CodeLifter::instance().disassemble(*Func).moveInto(TargetFunction));
    else
        LUTHIER_RETURN_ON_ERROR(LUTHIER_CREATE_ERROR(
            "Passed symbol is not of type kernel or device function."));

    llvm::SmallVector<llvm::MachineBasicBlock *, 4> MBBs;
    MBBs.push_back(MBB);
    for (unsigned int InstIdx = 0; InstIdx < TargetFunction.size(); InstIdx++) {
        LLVM_DEBUG(llvm::dbgs() << "+++++++++++++++++++++++++++++++++++++++++++++++"
                                   "+++++++++++++++++++++++++\n";);
        const auto &Inst = TargetFunction[InstIdx];
        auto MCInst = Inst.getMCInst();
        const unsigned Opcode = getPseudoOpcodeFromReal(MCInst.getOpcode());
        const llvm::MCInstrDesc &MCID = MCInstInfo->get(Opcode);
        bool IsDirectBranch = MCID.isBranch() && !MCID.isIndirectBranch();
        bool IsDirectBranchTarget =
            isAddressDirectBranchTarget(LCO, Inst.getLoadedDeviceAddress());
        LLVM_DEBUG(llvm::dbgs() << "Lifting and adding MC Inst: ";
        MCInst.dump_pretty(llvm::dbgs(), TargetInfo->getMCInstPrinter(),
                           " ", TargetInfo->getMCRegisterInfo());
        llvm::dbgs() << "\n";
        llvm::dbgs() << "Instruction idx: " << InstIdx << "\n";
        llvm::dbgs()
            << llvm::formatv("Loaded address of the instruction: {0:x}\n",
                             Inst.getLoadedDeviceAddress());
        llvm::dbgs()
            << llvm::formatv("Is branch? {0}\n", MCID.isBranch());
        llvm::dbgs() << llvm::formatv("Is indirect branch? {0}\n",
                                      MCID.isIndirectBranch()););

        if (IsDirectBranchTarget) {
            LLVM_DEBUG(llvm::dbgs() << "Instruction is a branch target.\n";);
            if (!MBB->empty()) {
                LLVM_DEBUG(llvm::dbgs()
                               << "Current MBB is not empty; Creating a new basic block\n");
                auto OldMBB = MBB;
                MBB = MF.CreateMachineBasicBlock();
                MF.push_back(MBB);
                MBBs.push_back(MBB);
                OldMBB->addSuccessor(MBB);
                // Branch targets mark the beginning of an MBB
                LLVM_DEBUG(llvm::dbgs()
                               << "*********************************************"
                                  "***************************\n");
            } else {
                LLVM_DEBUG(llvm::dbgs() << "Current MBB is empty; No new block created "
                                           "for the branch target.\n");
            }
            BranchTargetMBBs.insert({Inst.getLoadedDeviceAddress(), MBB});
            LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
                "Address {0:x} marks the beginning of MBB idx {1}.\n",
                Inst.getLoadedDeviceAddress(), MBB->getNumber()););
        }
        llvm::MachineInstrBuilder Builder =
            llvm::BuildMI(MBB, llvm::DebugLoc(), MCID);
        LR.MachineInstrToMCMap.insert(
            {Builder.getInstr(), const_cast<hsa::Instr *>(&Inst)});

        LLVM_DEBUG(llvm::dbgs() << "Number of operands according to MCID: "
                                << MCID.operands().size() << "\n";
        llvm::dbgs() << "Populating operands\n";);
        for (unsigned OpIndex = 0, E = MCInst.getNumOperands(); OpIndex < E;
             ++OpIndex) {
            const llvm::MCOperand &Op = MCInst.getOperand(OpIndex);
            if (Op.isReg()) {
                LLVM_DEBUG(llvm::dbgs() << "Resolving reg operand.\n");
                unsigned RegNum = RealToPseudoRegisterMapTable(Op.getReg());
                const bool IsDef = OpIndex < MCID.getNumDefs();
                unsigned Flags = 0;
                const llvm::MCOperandInfo &OpInfo = MCID.operands().begin()[OpIndex];
                if (IsDef && !OpInfo.isOptionalDef()) {
                    Flags |= llvm::RegState::Define;
                }
                LLVM_DEBUG(llvm::dbgs()
                               << "Adding register "
                               << llvm::printReg(RegNum,
                                                 MF.getSubtarget().getRegisterInfo())
                               << " with flags " << Flags << "\n";);
                Builder.addReg(RegNum, Flags);
            } else if (Op.isImm()) {
                LLVM_DEBUG(llvm::dbgs() << "Resolving an immediate operand.\n");
                // TODO: Resolve immediate load/store operands if they don't have
                // relocations associated with them (e.g. when they happen in the
                // text section)
                luthier::address_t InstAddr = Inst.getLoadedDeviceAddress();
                size_t InstSize = Inst.getSize();
                // Check if at any point in the instruction we need to apply
                // relocations
                bool RelocationApplied{false};
                for (luthier::address_t I = InstAddr; I <= InstAddr + InstSize; ++I) {
                    auto RelocationInfo = resolveRelocation(LCO, I);
                    LUTHIER_RETURN_ON_ERROR(RelocationInfo.takeError());
                    if (*RelocationInfo) {
                        auto &TargetSymbol = *RelocationInfo.get()->Symbol;

                        auto TargetSymbolType = TargetSymbol.getType();

                        auto Addend = RelocationInfo.get()->Relocation.getAddend();
                        LUTHIER_RETURN_ON_ERROR(Addend.takeError());

                        uint64_t Type = RelocationInfo.get()->Relocation.getType();

                        if (llvm::isa<hsa::LoadedCodeObjectKernel>(TargetSymbol) ||
                            llvm::isa<hsa::LoadedCodeObjectVariable>(TargetSymbol) ||
                            llvm::isa<hsa::LoadedCodeObjectExternSymbol>(TargetSymbol)) {
                            LLVM_DEBUG(
                                auto TargetSymbolAddress =
                                         TargetSymbol.getLoadedSymbolAddress();
                            LUTHIER_RETURN_ON_ERROR(TargetSymbolAddress.takeError());
                            LUTHIER_RETURN_ON_MOVE_INTO_FAIL(llvm::StringRef,
                                                             TargetSymbolName,
                                                             TargetSymbol.getName());
                            llvm::dbgs()
                                << llvm::formatv("Relocation is being resolved to the global "
                                                 "variables {0} at address {1:x}.\n",
                                                 TargetSymbolName,
                                                 reinterpret_cast<luthier::address_t>(
                                                     *TargetSymbolAddress)));
                            llvm::GlobalVariable *GV;
                            auto GVIter = LR.Variables.find(TargetSymbol);

                            auto TargetSymbolNameOrErr = TargetSymbol.getName();
                            LUTHIER_RETURN_ON_ERROR(TargetSymbolNameOrErr.takeError());

                            if (GVIter == LR.Variables.end() &&
                                TargetSymbol == LR.getKernel()) {
                                GV = &llvm::cast<llvm::GlobalVariable>(
                                    LR.getKernelMF().getFunction());
                            } else
                                GV = GVIter->second;

                            LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
                                GV != nullptr,
                                "Failed to find the GV associated with Symbol {0} in the LR.",
                                *TargetSymbolNameOrErr));

                            if (Type == llvm::ELF::R_AMDGPU_REL32_LO)
                                Type = llvm::SIInstrInfo::MO_GOTPCREL32_LO;
                            else if (Type == llvm::ELF::R_AMDGPU_REL32_HI)
                                Type = llvm::SIInstrInfo::MO_GOTPCREL32_HI;
                            Builder.addGlobalAddress(GV, *Addend, Type);
                        } else if (hsa::LoadedCodeObjectDeviceFunction
                            *TargetSymbolAsDevFunc = llvm::dyn_cast<
                            hsa::LoadedCodeObjectDeviceFunction>(
                            &TargetSymbol)) {
                            auto UsedMF = LR.Functions.find(TargetSymbolAsDevFunc);
                            LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
                                UsedMF != LR.Functions.end(),
                                "Failed to find the related function in map."));
                            llvm::Function &UsedF = UsedMF->second->getFunction();

                            // Add the function as the operand
                            if (Type == llvm::ELF::R_AMDGPU_REL32_LO)
                                Type = llvm::SIInstrInfo::MO_REL32_LO;
                            if (Type == llvm::ELF::R_AMDGPU_REL32_HI)
                                Type = llvm::SIInstrInfo::MO_REL32_HI;
                            Builder.addGlobalAddress(&UsedF, *Addend, Type);
                        } else {
                            // For now, we don't handle calling kernels from kernels
                            llvm_unreachable("not implemented");
                        }
                        RelocationApplied = true;
                        break;
                    }
                }
                if (!RelocationApplied && !IsDirectBranch) {
                    LLVM_DEBUG(
                        llvm::dbgs()
                            << "Relocation was not applied for the "
                               "immediate operand, and it is not a direct branch.\n";
                    llvm::dbgs() << "Adding the immediate operand directly to the "
                                    "instruction\n");
                    if (llvm::SIInstrInfo::isSOPK(*Builder)) {
                        LLVM_DEBUG(llvm::dbgs() << "Instruction is in SOPK format\n");
                        if (llvm::SIInstrInfo::sopkIsZext(Opcode)) {
                            auto Imm = static_cast<uint16_t>(Op.getImm());
                            LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
                                "Adding truncated imm value: {0}\n", Imm));
                            Builder.addImm(Imm);
                        } else {
                            auto Imm = static_cast<int16_t>(Op.getImm());
                            LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
                                "Adding truncated imm value: {0}\n", Imm));
                            Builder.addImm(Imm);
                        }
                    } else {
                        LLVM_DEBUG(llvm::dbgs()
                                       << llvm::formatv("Adding Imm: {0}\n", Op.getImm()));
                        Builder.addImm(Op.getImm());
                    }
                }

            } else if (!Op.isValid()) {
                llvm_unreachable("Operand is not set");
            } else {
                // TODO: implement floating point operands
                llvm_unreachable("Not yet implemented");
            }
        }
        // Create a (fake) memory operand to keep the machine verifier happy
        // when encountering image instructions
        if (llvm::SIInstrInfo::isImage(*Builder)) {
            llvm::MachinePointerInfo PtrInfo =
                llvm::MachinePointerInfo::getConstantPool(MF);
            auto *MMO = MF.getMachineMemOperand(
                PtrInfo,
                MCInstInfo->get(Builder->getOpcode()).mayLoad()
                ? llvm::MachineMemOperand::MOLoad
                : llvm::MachineMemOperand::MOStore,
                16, llvm::Align(8));
            Builder->addMemOperand(MF, MMO);
        }

        if (MCInst.getNumOperands() < MCID.NumOperands) {
            LLVM_DEBUG(llvm::dbgs() << "Must fixup instruction ";
            Builder->print(llvm::dbgs()); llvm::dbgs() << "\n";
            llvm::dbgs() << "Num explicit operands added so far: "
                         << MCInst.getNumOperands() << "\n";
            llvm::dbgs() << "Num explicit operands according to MCID: "
                         << MCID.NumOperands << "\n";);
            // Loop over missing explicit operands (if any) and fixup any missing
            for (unsigned int MissingExpOpIdx = MCInst.getNumOperands();
                 MissingExpOpIdx < MCID.NumOperands; MissingExpOpIdx++) {
                LLVM_DEBUG(llvm::dbgs()
                               << "Fixing up operand no " << MissingExpOpIdx << "\n";);
                auto OpType = MCID.operands()[MissingExpOpIdx].OperandType;
                if (OpType == llvm::MCOI::OPERAND_IMMEDIATE ||
                    OpType == llvm::AMDGPU::OPERAND_KIMM32) {
                    LLVM_DEBUG(llvm::dbgs() << "Added a 0-immediate operand.\n";);
                    Builder.addImm(0);
                }
            }
        }

        LUTHIER_RETURN_ON_ERROR(fixupBitsetInst(*Builder));
        LUTHIER_RETURN_ON_ERROR(verifyInstruction(Builder));
        LLVM_DEBUG(llvm::dbgs() << "Final form of the instruction (not final if "
                                   "it's a direct branch): ";
        Builder->print(llvm::dbgs()); llvm::dbgs() << "\n");
        // Basic Block resolving
        if (MCID.isTerminator()) {
            LLVM_DEBUG(llvm::dbgs()
                           << "Instruction is a terminator; Finishing basic block.\n");
            if (IsDirectBranch) {
                LLVM_DEBUG(llvm::dbgs() << "The terminator is a direct branch.\n");
                luthier::address_t BranchTarget;
                if (evaluateBranch(MCInst, Inst.getLoadedDeviceAddress(),
                                   Inst.getSize(), BranchTarget)) {
                    LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
                        "Address was resolved to {0:x}\n", BranchTarget));
                    if (!UnresolvedBranchMIs.contains(BranchTarget)) {
                        UnresolvedBranchMIs.insert({BranchTarget, {Builder.getInstr()}});
                    } else {
                        UnresolvedBranchMIs[BranchTarget].push_back(Builder.getInstr());
                    }
                } else {
                    LLVM_DEBUG(llvm::dbgs()
                                   << "Error resolving the target address of the branch\n");
                }
            }
            // if this is the last instruction in the stream, no need for creating
            // a new basic block
            if (InstIdx != TargetFunction.size() - 1) {
                LLVM_DEBUG(llvm::dbgs() << "Creating a new basic block.\n");
                auto OldMBB = MBB;
                MBB = MF.CreateMachineBasicBlock();
                MBBs.push_back(MBB);
                MF.push_back(MBB);
                // Don't add the next block to the list of successors if the
                // terminator is an unconditional branch
                if (!MCID.isUnconditionalBranch())
                    OldMBB->addSuccessor(MBB);
                LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
                    "Address {0:x} marks the beginning of MBB idx {1}.\n",
                    Inst.getLoadedDeviceAddress(), MBB->getNumber()););
            }
            LLVM_DEBUG(llvm::dbgs() << "*********************************************"
                                       "***************************\n");
        }
    }

    // Resolve the branch and target MIs/MBBs
    LLVM_DEBUG(llvm::dbgs() << "Resolving direct branch MIs\n");
    for (auto &[TargetAddress, BranchMIs] : UnresolvedBranchMIs) {
        LLVM_DEBUG(
            llvm::dbgs() << llvm::formatv(
                "Resolving MIs jumping to target address {0:x}.\n", TargetAddress));
        MBB = BranchTargetMBBs[TargetAddress];
        LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
            MBB != nullptr,
            "Failed to find the MachineBasicBlock associated with "
            "the branch target address {0:x}.",
            TargetAddress));
        for (auto &MI : BranchMIs) {
            LLVM_DEBUG(llvm::dbgs() << "Resolving branch for the instruction ";
            MI->print(llvm::dbgs()); llvm::dbgs() << "\n");
            MI->addOperand(llvm::MachineOperand::CreateMBB(MBB));
            MI->getParent()->addSuccessor(MBB);
            LLVM_DEBUG(llvm::dbgs() << llvm::formatv(
                "MBB {0:x} {1} was set as the target of the branch.\n",
                MBB, MBB->getName()));
            LLVM_DEBUG(llvm::dbgs() << "Final branch instruction: ";
            MI->print(llvm::dbgs()); llvm::dbgs() << "\n");
        }
    }

    LLVM_DEBUG(llvm::dbgs() << "*********************************************"
                               "***************************\n");

    MF.getRegInfo().freezeReservedRegs();

    if (MF.getFunction().getCallingConv() == llvm::CallingConv::AMDGPU_KERNEL) {
        // Manually set the stack frame size if the function is a kernel
        // TODO: dynamic stack kernels
        const auto *KernelSymbol =
            llvm::dyn_cast<hsa::LoadedCodeObjectKernel>(&Symbol);
        LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
            KernelSymbol != nullptr,
            "LLVM Machine Function {0} was incorrectly lifted to a kernel",
            MF.getName()));
        auto KernelMD = KernelSymbol->getKernelMetadata();
        LLVM_DEBUG(llvm::dbgs() << "Stack size according to the metadata: "
                                << KernelMD.PrivateSegmentFixedSize << "\n");
        if (KernelMD.PrivateSegmentFixedSize != 0) {
            MF.getFrameInfo().CreateFixedObject(KernelMD.PrivateSegmentFixedSize, 0,
                                                true);
            MF.getFrameInfo().setStackSize(KernelMD.PrivateSegmentFixedSize);
            LLVM_DEBUG(llvm::dbgs()
                           << "Stack size: " << MF.getFrameInfo().getStackSize() << "\n");
        }
    }

    // Populate the properties of MF
    llvm::MachineFunctionProperties &Properties = MF.getProperties();
    Properties.set(llvm::MachineFunctionProperties::Property::NoVRegs);
    Properties.reset(llvm::MachineFunctionProperties::Property::IsSSA);
    Properties.set(llvm::MachineFunctionProperties::Property::NoPHIs);
    Properties.set(llvm::MachineFunctionProperties::Property::TracksLiveness);
    Properties.set(llvm::MachineFunctionProperties::Property::Selected);

    LLVM_DEBUG(llvm::dbgs() << "Final form of the Machine function:\n";
    MF.print(llvm::dbgs());
    llvm::dbgs() << "\n"
                 << "*********************************************"
                    "***************************\n";);
    return llvm::Error::success();
}

llvm::Expected<const LiftedRepresentation &>
luthier::CodeLifter::lift(const hsa::LoadedCodeObjectKernel &KernelSymbol) {
    std::lock_guard Lock(CacheMutex);
    if (!LiftedKernelSymbols.contains(KernelSymbol)) {
        // Lift the kernel if not already lifted
        llvm::TimeTraceScope Scope("Lifting Kernel");
        // Start a lifted representation
        std::unique_ptr<LiftedRepresentation> LR(new LiftedRepresentation());
        // Initialize the LR
        LUTHIER_RETURN_ON_ERROR(initLR(*LR, KernelSymbol));
        hsa::LoadedCodeObject LCO(LR->LCO);

        // Create Global Variables associated with the LCO
        llvm::SmallVector<std::unique_ptr<hsa::LoadedCodeObjectSymbol>, 4>
            GlobalVariables;
        LUTHIER_RETURN_ON_ERROR(LCO.getVariableSymbols(GlobalVariables));
        LUTHIER_RETURN_ON_ERROR(LCO.getExternalSymbols(GlobalVariables));
        for (auto &GV : GlobalVariables) {
            LUTHIER_RETURN_ON_ERROR(initLiftedGlobalVariableEntry(LCO, *GV, *LR));
        }
        // Create Kernel entries for the LCO
        llvm::SmallVector<std::unique_ptr<hsa::LoadedCodeObjectSymbol>> Kernels;
        LUTHIER_RETURN_ON_ERROR(LCO.getKernelSymbols(Kernels));
        for (const auto &Kernel : Kernels) {
            if (*Kernel == KernelSymbol)
                LUTHIER_RETURN_ON_ERROR(initLiftedKernelEntry(KernelSymbol, *LR));
            else
                LUTHIER_RETURN_ON_ERROR(
                    initLiftedGlobalVariableEntry(LCO, *Kernel, *LR));
        }
        // Create device function entries for this LCO
        llvm::SmallVector<std::unique_ptr<hsa::LoadedCodeObjectSymbol>, 4>
            DeviceFuncs;
        LUTHIER_RETURN_ON_ERROR(LCO.getDeviceFunctionSymbols(DeviceFuncs));
        for (const auto &Func : DeviceFuncs) {
            LUTHIER_RETURN_ON_ERROR(initLiftedDeviceFunctionEntry(
                *llvm::dyn_cast<hsa::LoadedCodeObjectDeviceFunction>(Func.get()),
                *LR));
        }
        // Now that all global objects are initialized, we can now populate
        // the target kernel's instructions

        LUTHIER_RETURN_ON_ERROR(liftFunction(KernelSymbol, LR->getKernelMF(), *LR));

        for (const auto &[Func, MF] : LR->functions()) {
            LUTHIER_RETURN_ON_ERROR(liftFunction(*Func, *MF, *LR));
        }
        LiftedKernelSymbols.emplace(
            llvm::unique_dyn_cast<hsa::LoadedCodeObjectKernel>(
                KernelSymbol.clone()),
            std::move(LR));
    }
    return *LiftedKernelSymbols.find(&KernelSymbol)->second;
}

llvm::Expected<std::unique_ptr<LiftedRepresentation>>
CodeLifter::cloneRepresentation(const LiftedRepresentation &SrcLR) {
    llvm::TimeTraceScope ProfilerScope("Lifted Representation Cloning");
    // Since we're going to use the SrcLR's context, acquire its lock
    auto Lock = SrcLR.getLock();
    // Construct the output
    std::unique_ptr<LiftedRepresentation> DestLR(new LiftedRepresentation());
    // The cloned LiftedRepresentation will share the context and the
    // lifted primitive
    DestLR->Context = SrcLR.Context;
    // This VMap will be populated by a mapping between the original global
    // objects and their cloned version. This will be useful when populating
    // the related functions and related global variable maps of the cloned
    // LiftedRepresentation
    llvm::ValueToValueMapTy VMap;
    // This map helps us populate the MachineInstr to hsa::Instr map
    llvm::DenseMap<llvm::MachineInstr *, llvm::MachineInstr *> SrcToDstInstrMap;
    const llvm::Module &SrcModule = *SrcLR.Module;
    const llvm::MachineModuleInfo &SrcMMI = SrcLR.MMIWP->getMMI();
    // Clone the Module and the MMI
    // Create a new target machine for the MMI
    DestLR->Module = llvm::CloneModule(SrcModule, VMap);
    DestLR->LCO = SrcLR.LCO;
    auto ISA = hsa::LoadedCodeObject(DestLR->LCO).getISA();
    LUTHIER_RETURN_ON_ERROR(ISA.takeError());
    LUTHIER_RETURN_ON_ERROR(
        TargetManager::instance().createTargetMachine(*ISA).moveInto(DestLR->TM));
    DestLR->Module->setDataLayout(DestLR->TM->createDataLayout());
    DestLR->MMIWP =
        std::make_unique<llvm::MachineModuleInfoWrapperPass>(DestLR->TM.get());
    LUTHIER_RETURN_ON_ERROR(
        cloneMMI(SrcMMI, SrcModule, VMap, DestLR->getMMI(), &SrcToDstInstrMap));

    DestLR->Kernel =
        llvm::unique_dyn_cast<hsa::LoadedCodeObjectKernel>(SrcLR.Kernel->clone());

    auto DestKernelEntry = VMap.find(&SrcLR.KernelMF->getFunction());
    LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
        DestKernelEntry != VMap.end(),
        "Failed to find the matching LLVM Function {0} during "
        "Lifted Representation cloning.",
        SrcLR.KernelMF->getFunction()));

    DestLR->KernelMF = DestLR->getMMI().getMachineFunction(
        *cast<llvm::Function>(DestKernelEntry->second));

    // With all Modules and MMIs cloned, we need to populate the related
    // functions and related global variables. We use the VMap to do this
    for (const auto &[GVHandle, GV] : SrcLR.Variables) {
        auto GVDestEntry = VMap.find(GV);
        LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
            GVDestEntry != VMap.end(),
            "Failed to find the matching LLVM Global "
            "variable for {0} during Lifted Representation cloning.",
            *GV));
        auto *DestGV = cast<llvm::GlobalVariable>(GVDestEntry->second);
        DestLR->Variables.emplace(GVHandle->clone(), DestGV);
    }
    for (const auto &[FuncSymbol, SrcMF] : SrcLR.Functions) {
        auto FDestEntry = VMap.find(&SrcMF->getFunction());
        LUTHIER_RETURN_ON_ERROR(LUTHIER_ERROR_CHECK(
            FDestEntry != VMap.end(),
            "Failed to find the matching LLVM Function {0} during "
            "Lifted Representation cloning.",
            SrcMF->getFunction()));
        auto *DestF = cast<llvm::Function>(FDestEntry->second);
        // Get the MMI of the dest function
        auto DestMF = DestLR->getMMI().getMachineFunction(*DestF);
        DestLR->Functions.emplace(
            llvm::unique_dyn_cast<hsa::LoadedCodeObjectDeviceFunction>(
                FuncSymbol->clone()),
            DestMF);
    }
    // Finally, populate the instruction map, the Live-ins map, and the global
    // value uses
    for (const auto &[SrcMI, HSAInst] : SrcLR.MachineInstrToMCMap) {
        DestLR->MachineInstrToMCMap.insert({SrcToDstInstrMap[SrcMI], HSAInst});
    }

    return DestLR;
}

} // namespace luthier