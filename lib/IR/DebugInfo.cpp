//===--- DebugInfo.cpp - Debug Information Helper Classes -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the helper classes used to build and interpret debug
// information in LLVM IR form.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/DebugInfo.h"
#include "LLVMContextImpl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/GVMaterializer.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;
using namespace llvm::dwarf;

DISubprogram llvm::getDISubprogram(const MDNode *Scope) {
  if (auto *LocalScope = dyn_cast_or_null<MDLocalScope>(Scope))
    return LocalScope->getSubprogram();
  return nullptr;
}

DISubprogram llvm::getDISubprogram(const Function *F) {
  // We look for the first instr that has a debug annotation leading back to F.
  for (auto &BB : *F) {
    auto Inst = std::find_if(BB.begin(), BB.end(), [](const Instruction &Inst) {
      return Inst.getDebugLoc();
    });
    if (Inst == BB.end())
      continue;
    DebugLoc DLoc = Inst->getDebugLoc();
    const MDNode *Scope = DLoc.getInlinedAtScope();
    DISubprogram Subprogram = getDISubprogram(Scope);
    return Subprogram->describes(F) ? Subprogram : DISubprogram();
  }

  return DISubprogram();
}

DICompositeType llvm::getDICompositeType(DIType T) {
  if (auto *C = dyn_cast_or_null<MDCompositeTypeBase>(T))
    return C;

  if (auto *D = dyn_cast_or_null<MDDerivedTypeBase>(T)) {
    // This function is currently used by dragonegg and dragonegg does
    // not generate identifier for types, so using an empty map to resolve
    // DerivedFrom should be fine.
    DITypeIdentifierMap EmptyMap;
    return getDICompositeType(D->getBaseType().resolve(EmptyMap));
  }

  return nullptr;
}

DITypeIdentifierMap
llvm::generateDITypeIdentifierMap(const NamedMDNode *CU_Nodes) {
  DITypeIdentifierMap Map;
  for (unsigned CUi = 0, CUe = CU_Nodes->getNumOperands(); CUi != CUe; ++CUi) {
    auto *CU = cast<MDCompileUnit>(CU_Nodes->getOperand(CUi));
    DIArray Retain = CU->getRetainedTypes();
    for (unsigned Ti = 0, Te = Retain.size(); Ti != Te; ++Ti) {
      if (!isa<MDCompositeType>(Retain[Ti]))
        continue;
      auto *Ty = cast<MDCompositeType>(Retain[Ti]);
      if (MDString *TypeId = Ty->getRawIdentifier()) {
        // Definition has priority over declaration.
        // Try to insert (TypeId, Ty) to Map.
        std::pair<DITypeIdentifierMap::iterator, bool> P =
            Map.insert(std::make_pair(TypeId, Ty));
        // If TypeId already exists in Map and this is a definition, replace
        // whatever we had (declaration or definition) with the definition.
        if (!P.second && !Ty->isForwardDecl())
          P.first->second = Ty;
      }
    }
  }
  return Map;
}

//===----------------------------------------------------------------------===//
// DebugInfoFinder implementations.
//===----------------------------------------------------------------------===//

void DebugInfoFinder::reset() {
  CUs.clear();
  SPs.clear();
  GVs.clear();
  TYs.clear();
  Scopes.clear();
  NodesSeen.clear();
  TypeIdentifierMap.clear();
  TypeMapInitialized = false;
}

void DebugInfoFinder::InitializeTypeMap(const Module &M) {
  if (!TypeMapInitialized)
    if (NamedMDNode *CU_Nodes = M.getNamedMetadata("llvm.dbg.cu")) {
      TypeIdentifierMap = generateDITypeIdentifierMap(CU_Nodes);
      TypeMapInitialized = true;
    }
}

void DebugInfoFinder::processModule(const Module &M) {
  InitializeTypeMap(M);
  if (NamedMDNode *CU_Nodes = M.getNamedMetadata("llvm.dbg.cu")) {
    for (unsigned i = 0, e = CU_Nodes->getNumOperands(); i != e; ++i) {
      DICompileUnit CU = cast<MDCompileUnit>(CU_Nodes->getOperand(i));
      addCompileUnit(CU);
      for (DIGlobalVariable DIG : CU->getGlobalVariables()) {
        if (addGlobalVariable(DIG)) {
          processScope(DIG->getScope());
          processType(DIG->getType().resolve(TypeIdentifierMap));
        }
      }
      for (auto *SP : CU->getSubprograms())
        processSubprogram(SP);
      for (auto *ET : CU->getEnumTypes())
        processType(ET);
      for (auto *RT : CU->getRetainedTypes())
        processType(RT);
      for (DIImportedEntity Import : CU->getImportedEntities()) {
        auto *Entity = Import->getEntity().resolve(TypeIdentifierMap);
        if (auto *T = dyn_cast<MDType>(Entity))
          processType(T);
        else if (auto *SP = dyn_cast<MDSubprogram>(Entity))
          processSubprogram(SP);
        else if (auto *NS = dyn_cast<MDNamespace>(Entity))
          processScope(NS->getScope());
      }
    }
  }
}

void DebugInfoFinder::processLocation(const Module &M, DILocation Loc) {
  if (!Loc)
    return;
  InitializeTypeMap(M);
  processScope(Loc->getScope());
  processLocation(M, Loc->getInlinedAt());
}

void DebugInfoFinder::processType(DIType DT) {
  if (!addType(DT))
    return;
  processScope(DT->getScope().resolve(TypeIdentifierMap));
  if (auto *DCT = dyn_cast<MDCompositeTypeBase>(DT)) {
    processType(DCT->getBaseType().resolve(TypeIdentifierMap));
    if (auto *ST = dyn_cast<MDSubroutineType>(DCT)) {
      for (MDTypeRef Ref : ST->getTypeArray())
        processType(Ref.resolve(TypeIdentifierMap));
      return;
    }
    for (Metadata *D : DCT->getElements()) {
      if (auto *T = dyn_cast<MDType>(D))
        processType(T);
      else if (DISubprogram SP = dyn_cast<MDSubprogram>(D))
        processSubprogram(SP);
    }
  } else if (auto *DDT = dyn_cast<MDDerivedTypeBase>(DT)) {
    processType(DDT->getBaseType().resolve(TypeIdentifierMap));
  }
}

void DebugInfoFinder::processScope(DIScope Scope) {
  if (!Scope)
    return;
  if (DIType Ty = dyn_cast<MDType>(Scope)) {
    processType(Ty);
    return;
  }
  if (DICompileUnit CU = dyn_cast<MDCompileUnit>(Scope)) {
    addCompileUnit(CU);
    return;
  }
  if (DISubprogram SP = dyn_cast<MDSubprogram>(Scope)) {
    processSubprogram(SP);
    return;
  }
  if (!addScope(Scope))
    return;
  if (auto *LB = dyn_cast<MDLexicalBlockBase>(Scope)) {
    processScope(LB->getScope());
  } else if (auto *NS = dyn_cast<MDNamespace>(Scope)) {
    processScope(NS->getScope());
  }
}

void DebugInfoFinder::processSubprogram(DISubprogram SP) {
  if (!addSubprogram(SP))
    return;
  processScope(SP->getScope().resolve(TypeIdentifierMap));
  processType(SP->getType());
  for (auto *Element : SP->getTemplateParams()) {
    if (auto *TType = dyn_cast<MDTemplateTypeParameter>(Element)) {
      processType(TType->getType().resolve(TypeIdentifierMap));
    } else if (auto *TVal = dyn_cast<MDTemplateValueParameter>(Element)) {
      processType(TVal->getType().resolve(TypeIdentifierMap));
    }
  }
}

void DebugInfoFinder::processDeclare(const Module &M,
                                     const DbgDeclareInst *DDI) {
  MDNode *N = dyn_cast<MDNode>(DDI->getVariable());
  if (!N)
    return;
  InitializeTypeMap(M);

  DIVariable DV = dyn_cast<MDLocalVariable>(N);
  if (!DV)
    return;

  if (!NodesSeen.insert(DV).second)
    return;
  processScope(DV->getScope());
  processType(DV->getType().resolve(TypeIdentifierMap));
}

void DebugInfoFinder::processValue(const Module &M, const DbgValueInst *DVI) {
  MDNode *N = dyn_cast<MDNode>(DVI->getVariable());
  if (!N)
    return;
  InitializeTypeMap(M);

  DIVariable DV = dyn_cast<MDLocalVariable>(N);
  if (!DV)
    return;

  if (!NodesSeen.insert(DV).second)
    return;
  processScope(DV->getScope());
  processType(DV->getType().resolve(TypeIdentifierMap));
}

bool DebugInfoFinder::addType(DIType DT) {
  if (!DT)
    return false;

  if (!NodesSeen.insert(DT).second)
    return false;

  TYs.push_back(DT);
  return true;
}

bool DebugInfoFinder::addCompileUnit(DICompileUnit CU) {
  if (!CU)
    return false;
  if (!NodesSeen.insert(CU).second)
    return false;

  CUs.push_back(CU);
  return true;
}

bool DebugInfoFinder::addGlobalVariable(DIGlobalVariable DIG) {
  if (!DIG)
    return false;

  if (!NodesSeen.insert(DIG).second)
    return false;

  GVs.push_back(DIG);
  return true;
}

bool DebugInfoFinder::addSubprogram(DISubprogram SP) {
  if (!SP)
    return false;

  if (!NodesSeen.insert(SP).second)
    return false;

  SPs.push_back(SP);
  return true;
}

bool DebugInfoFinder::addScope(DIScope Scope) {
  if (!Scope)
    return false;
  // FIXME: Ocaml binding generates a scope with no content, we treat it
  // as null for now.
  if (Scope->getNumOperands() == 0)
    return false;
  if (!NodesSeen.insert(Scope).second)
    return false;
  Scopes.push_back(Scope);
  return true;
}

bool llvm::stripDebugInfo(Function &F) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (I.getDebugLoc()) {
        Changed = true;
        I.setDebugLoc(DebugLoc());
      }
    }
  }
  return Changed;
}

bool llvm::StripDebugInfo(Module &M) {
  bool Changed = false;

  // Remove all of the calls to the debugger intrinsics, and remove them from
  // the module.
  if (Function *Declare = M.getFunction("llvm.dbg.declare")) {
    while (!Declare->use_empty()) {
      CallInst *CI = cast<CallInst>(Declare->user_back());
      CI->eraseFromParent();
    }
    Declare->eraseFromParent();
    Changed = true;
  }

  if (Function *DbgVal = M.getFunction("llvm.dbg.value")) {
    while (!DbgVal->use_empty()) {
      CallInst *CI = cast<CallInst>(DbgVal->user_back());
      CI->eraseFromParent();
    }
    DbgVal->eraseFromParent();
    Changed = true;
  }

  for (Module::named_metadata_iterator NMI = M.named_metadata_begin(),
         NME = M.named_metadata_end(); NMI != NME;) {
    NamedMDNode *NMD = NMI;
    ++NMI;
    if (NMD->getName().startswith("llvm.dbg.")) {
      NMD->eraseFromParent();
      Changed = true;
    }
  }

  for (Function &F : M)
    Changed |= stripDebugInfo(F);

  if (GVMaterializer *Materializer = M.getMaterializer())
    Materializer->setStripDebugInfo();

  return Changed;
}

unsigned llvm::getDebugMetadataVersionFromModule(const Module &M) {
  if (auto *Val = mdconst::dyn_extract_or_null<ConstantInt>(
          M.getModuleFlag("Debug Info Version")))
    return Val->getZExtValue();
  return 0;
}

llvm::DenseMap<const llvm::Function *, llvm::DISubprogram>
llvm::makeSubprogramMap(const Module &M) {
  DenseMap<const Function *, DISubprogram> R;

  NamedMDNode *CU_Nodes = M.getNamedMetadata("llvm.dbg.cu");
  if (!CU_Nodes)
    return R;

  for (MDNode *N : CU_Nodes->operands()) {
    DICompileUnit CUNode = cast<MDCompileUnit>(N);
    for (DISubprogram SP : CUNode->getSubprograms()) {
      if (Function *F = SP->getFunction())
        R.insert(std::make_pair(F, SP));
    }
  }
  return R;
}
