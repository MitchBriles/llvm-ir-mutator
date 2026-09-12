//===-- main.cpp - llvm-ir-mutator command line -------------------------===//
//
// Streams mutants of a seed module to stdout, separated by NUL by default, so
// a consumer can read records without escaping anything. The stream is endless
// unless --count bounds it.
//
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "Mutator.h"

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <random>

using namespace llvm;

namespace {

cl::OptionCategory MutatorCat("llvm-ir-mutator options");

cl::opt<std::string> InputFile(cl::Positional, cl::desc("<seed .ll or .bc>"),
                               cl::cat(MutatorCat));

cl::opt<uint64_t> SeedOpt("seed",
                          cl::desc("Master seed (default: nondeterministic)"),
                          cl::init(0), cl::cat(MutatorCat));

cl::opt<uint64_t>
    MutantSeedOpt("mutant-seed",
                  cl::desc("Reproduce the single mutant with this per-mutant "
                           "seed, as printed in a mutant's header"),
                  cl::init(0), cl::cat(MutatorCat));

cl::opt<uint64_t> Count("count",
                        cl::desc("Stop after N mutants (default: endless)"),
                        cl::init(0), cl::cat(MutatorCat));

cl::opt<unsigned>
    Mutations("mutations",
              cl::desc("Mutations per mutant (0: uniform in [1,5])"),
              cl::init(0), cl::cat(MutatorCat));

cl::opt<std::string>
    Sep("sep", cl::desc("Record separator: nul, nl, none, or a literal string"),
        cl::init("nul"), cl::cat(MutatorCat));

cl::list<std::string> Only("only", cl::CommaSeparated,
                           cl::desc("Run only these pathways"),
                           cl::cat(MutatorCat));

cl::list<std::string> Disable("disable", cl::CommaSeparated,
                              cl::desc("Exclude these pathways"),
                              cl::cat(MutatorCat));

cl::opt<std::string>
    Rollback("rollback",
             cl::desc("'step': verify and roll back each mutation (default); "
                      "'mutant': verify once per mutant"),
             cl::init("step"), cl::cat(MutatorCat));

cl::opt<uint64_t> MaxSize("max-size",
                          cl::desc("Module object budget (0: seed + headroom)"),
                          cl::init(0), cl::cat(MutatorCat));

cl::opt<std::string> Emit("emit", cl::desc("Output format: ll or bc"),
                          cl::init("ll"), cl::cat(MutatorCat));

cl::opt<bool> BaselineOps("baseline-ops",
                          cl::desc("Restrict `inject` to LLVM's default "
                                   "operation set, for diversity comparisons"),
                          cl::init(false), cl::cat(MutatorCat));

cl::opt<bool>
    Alive2Safe("alive2-safe",
               cl::desc("Emit only constructs Alive2 can reason about: no "
                        "volatile accesses, no noalias, and no afn/arcp/"
                        "contract/reassoc fast-math flags"),
               cl::init(false), cl::cat(MutatorCat));

cl::opt<bool> Report("report",
                     cl::desc("Print a per-pathway table to stderr on exit"),
                     cl::init(false), cl::cat(MutatorCat));

cl::opt<bool> List("list", cl::desc("List pathways and weights, then exit"),
                   cl::init(false), cl::cat(MutatorCat));

cl::opt<bool> NoHeader("no-header",
                       cl::desc("Omit the per-mutant reproducibility comment"),
                       cl::init(false), cl::cat(MutatorCat));

std::string separator() {
  if (Sep == "nul")
    return std::string(1, '\0');
  if (Sep == "nl")
    return "\n";
  if (Sep == "none")
    return "";
  return Sep;
}

void printStats(const mutator::Mutator &M) {
  errs() << "\npathway      chosen   applied     no-op   invalid\n";
  errs() << "---------------------------------------------------\n";
  auto Names = M.names();
  auto S = M.stats();
  for (size_t I = 0; I < Names.size(); ++I)
    errs() << format(
        "%-12s %8llu  %8llu  %8llu  %8llu\n", Names[I].c_str(),
        (unsigned long long)S[I].Chosen, (unsigned long long)S[I].Applied,
        (unsigned long long)S[I].NoOp, (unsigned long long)S[I].Invalid);
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM Init(argc, argv);
  cl::HideUnrelatedOptions(MutatorCat);
  cl::ParseCommandLineOptions(argc, argv, "llvm-ir-mutator\n");

  if (List) {
    for (const auto &Spec : mutator::config::strategies())
      outs() << format("%-12s weight %3llu  %s\n", Spec.Name,
                       (unsigned long long)Spec.Weight, Spec.Doc);
    return 0;
  }

  if (InputFile.empty()) {
    errs() << argv[0] << ": a seed .ll or .bc file is required\n";
    return 1;
  }

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> Seed = parseIRFile(InputFile, Err, Ctx);
  if (!Seed) {
    Err.print(argv[0], errs());
    return 1;
  }
  if (verifyModule(*Seed, &errs())) {
    errs() << argv[0] << ": seed module does not verify\n";
    return 1;
  }

  mutator::Options Opts;
  Opts.Seed =
      SeedOpt.getNumOccurrences() ? SeedOpt.getValue() : std::random_device{}();
  Opts.Mutations = Mutations;
  Opts.RollbackPerStep = Rollback != "mutant";
  Opts.BaselineOps = BaselineOps;
  Opts.Alive2Safe = Alive2Safe;
  Opts.MaxSize = MaxSize;
  Opts.Only.assign(Only.begin(), Only.end());
  Opts.Disable.assign(Disable.begin(), Disable.end());

  for (const auto &Name : {Opts.Only, Opts.Disable})
    for (const auto &N : Name)
      if (none_of(mutator::config::strategies(),
                  [&](const auto &S) { return N == S.Name; })) {
        errs() << argv[0] << ": unknown pathway '" << N << "' (see --list)\n";
        return 1;
      }

  errs() << "; master seed " << Opts.Seed << "\n";

  mutator::Mutator M(*Seed, Opts);
  // A master engine hands each mutant its own seed, which is printed in the
  // mutant, so any single mutant can be reproduced on its own.
  std::mt19937_64 Master(Opts.Seed);
  const std::string Separator = separator();
  const bool Bitcode = Emit == "bc";

  // --mutant-seed replays one mutant straight from the value in its header,
  // bypassing the master engine that would otherwise derive it.
  const bool Replay = MutantSeedOpt.getNumOccurrences() != 0;
  const uint64_t Limit = Replay && Count == 0 ? 1 : Count;

  std::vector<StringRef> Applied;
  for (uint64_t I = 0; Limit == 0 || I < Limit; ++I) {
    uint64_t MutantSeed = Replay ? MutantSeedOpt.getValue() : Master();
    std::unique_ptr<Module> Mutant = M.mutate(MutantSeed, Applied);

    if (Bitcode) {
      WriteBitcodeToFile(*Mutant, outs());
    } else {
      std::string Text;
      raw_string_ostream OS(Text);
      if (!NoHeader) {
        OS << "; mutant=" << I << " seed=" << MutantSeed << " applied=";
        for (size_t J = 0; J < Applied.size(); ++J)
          OS << (J ? "," : "") << Applied[J];
        if (Applied.empty())
          OS << "none";
        OS << "\n";
      }
      Mutant->print(OS, nullptr);
      outs() << Text;
    }
    outs().write(Separator.data(), Separator.size());
    outs().flush();
  }

  if (Report)
    printStats(M);
  return 0;
}
