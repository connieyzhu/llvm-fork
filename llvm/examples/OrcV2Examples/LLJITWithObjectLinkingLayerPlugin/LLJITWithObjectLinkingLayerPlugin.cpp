//===--------------- LLJITWithCustomObjectLinkingLayer.cpp ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file shows how to switch LLJIT to use a custom object linking layer (we
// use ObjectLinkingLayer, which is backed by JITLink, as an example).
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringMap.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "../ExampleModules.h"

using namespace llvm;
using namespace llvm::orc;

ExitOnError ExitOnErr;

const llvm::StringRef TestMod =
    R"(
  define i32 @callee() {
  entry:
    ret i32 7
  }

  define i32 @entry() {
  entry:
    %0 = call i32 @callee()
    ret i32 %0
  }
)";

class MyPlugin : public ObjectLinkingLayer::Plugin {
public:
  // The modifyPassConfig callback gives us a chance to inspect the
  // MaterializationResponsibility and target triple for the object being
  // linked, then add any JITLink passes that we would like to run on the
  // link graph. A pass is just a function object that is callable as
  // Error(jitlink::LinkGraph&). In this case we will add two passes
  // defined as lambdas that call the printLinkerGraph method on our
  // plugin: One to run before the linker applies fixups and another to
  // run afterwards.
  void modifyPassConfig(MaterializationResponsibility &MR, const Triple &TT,
                        jitlink::PassConfiguration &Config) override {
    Config.PostPrunePasses.push_back([this](jitlink::LinkGraph &G) -> Error {
      printLinkGraph(G, "Before fixup:");
      return Error::success();
    });
    Config.PostFixupPasses.push_back([this](jitlink::LinkGraph &G) -> Error {
      printLinkGraph(G, "After fixup:");
      return Error::success();
    });
  }

  void notifyLoaded(MaterializationResponsibility &MR) override {
    dbgs() << "Loading object defining " << MR.getSymbols() << "\n";
  }

  Error notifyEmitted(MaterializationResponsibility &MR) override {
    dbgs() << "Emitted object defining " << MR.getSymbols() << "\n";
    return Error::success();
  }

  Error notifyFailed(MaterializationResponsibility &MR) override {
    return Error::success();
  }

  Error notifyRemovingResources(ResourceKey K) override {
    return Error::success();
  }

  void notifyTransferringResources(ResourceKey DstKey,
                                   ResourceKey SrcKey) override {}

private:
  void printLinkGraph(jitlink::LinkGraph &G, StringRef Title) {
    constexpr JITTargetAddress LineWidth = 16;

    dbgs() << "--- " << Title << "---\n";
    for (auto &S : G.sections()) {
      dbgs() << "  section: " << S.getName() << "\n";
      for (auto *B : S.blocks()) {
        dbgs() << "    block@" << formatv("{0:x16}", B->getAddress()) << ":\n";

        if (B->isZeroFill())
          continue;

        JITTargetAddress InitAddr = B->getAddress() & ~(LineWidth - 1);
        JITTargetAddress StartAddr = B->getAddress();
        JITTargetAddress EndAddr = B->getAddress() + B->getSize();
        auto *Data = reinterpret_cast<const uint8_t *>(B->getContent().data());

        for (JITTargetAddress CurAddr = InitAddr; CurAddr != EndAddr;
             ++CurAddr) {
          if (CurAddr % LineWidth == 0)
            dbgs() << "    " << formatv("{0:x16}", CurAddr) << ": ";
          if (CurAddr < StartAddr)
            dbgs() << "   ";
          else
            dbgs() << formatv("{0:x-2}", Data[CurAddr - StartAddr]) << " ";
          if (CurAddr % LineWidth == LineWidth - 1)
            dbgs() << "\n";
        }
        if (EndAddr % LineWidth != 0)
          dbgs() << "\n";
        dbgs() << "\n";
      }
    }
  }
};

int main(int argc, char *argv[]) {
  // Initialize LLVM.
  InitLLVM X(argc, argv);

  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  cl::ParseCommandLineOptions(argc, argv, "LLJITWithObjectLinkingLayerPlugin");
  ExitOnErr.setBanner(std::string(argv[0]) + ": ");

  // Detect the host and set code model to small.
  auto JTMB = ExitOnErr(JITTargetMachineBuilder::detectHost());
  JTMB.setCodeModel(CodeModel::Small);

  // Create an LLJIT instance with an ObjectLinkingLayer as the base layer.
  // We attach our plugin in to the newly created ObjectLinkingLayer before
  // returning it.
  auto J = ExitOnErr(
      LLJITBuilder()
          .setJITTargetMachineBuilder(std::move(JTMB))
          .setObjectLinkingLayerCreator(
              [&](ExecutionSession &ES, const Triple &TT) {
                // Create ObjectLinkingLayer.
                auto ObjLinkingLayer = std::make_unique<ObjectLinkingLayer>(
                    ES, std::make_unique<jitlink::InProcessMemoryManager>());
                // Add an instance of our plugin.
                ObjLinkingLayer->addPlugin(std::make_unique<MyPlugin>());
                return std::move(ObjLinkingLayer);
              })
          .create());

  auto M = ExitOnErr(parseExampleModule(TestMod, "test-module"));

  ExitOnErr(J->addIRModule(std::move(M)));

  // Look up the JIT'd function, cast it to a function pointer, then call it.
  auto EntrySym = ExitOnErr(J->lookup("entry"));
  auto *Entry = (int (*)())EntrySym.getAddress();

  int Result = Entry();
  outs() << "---Result---\n"
         << "entry() = " << Result << "\n";

  return 0;
}
