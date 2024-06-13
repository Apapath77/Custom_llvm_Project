#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define RISCV_MACHINEINSTR_PRINTER_PASS_NAME "RISC-V Machine Instruction Hazards Pass"

namespace {

class RISCVMachineInstrHazards : public MachineFunctionPass {
public:
    static char ID;

    RISCVMachineInstrHazards() : MachineFunctionPass(ID) {
        initializeRISCVMachineInstrHazardsPass(*PassRegistry::getPassRegistry());
    }

    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override { return RISCV_MACHINEINSTR_PRINTER_PASS_NAME; }

private:
    void insertNOOPNInstructions(MachineBasicBlock &MBB, const RISCVInstrInfo *TII);
    bool hasDataHazard(const MachineInstr &MI, const SmallSet<unsigned, 32> &liveRegs, const SmallSet<unsigned, 32> &defRegs);
    void updateLiveAndDefRegs(const MachineInstr &MI, SmallSet<unsigned, 32> &liveRegs, SmallSet<unsigned, 32> &defRegs);
    bool hasMemoryDependency(const MachineInstr &MI1, const MachineInstr &MI2);
};

char RISCVMachineInstrHazards::ID = 0;

bool RISCVMachineInstrHazards::runOnMachineFunction(MachineFunction &MF) {
    errs() << "Running RISC-V Machine Instruction Hazards Pass on function: " << MF.getName() << "\n";

    const RISCVSubtarget &STI = MF.getSubtarget<RISCVSubtarget>();
    const RISCVInstrInfo *TII = STI.getInstrInfo();

    for (MachineBasicBlock &MBB : MF) {
        errs() << "  Processing Machine Basic Block: " << MBB.getName() << "\n";
        insertNOOPNInstructions(MBB, TII);

        for (MachineInstr &MI : MBB) {
            errs() << "    Machine Instruction: ";
            MI.print(errs());
            errs() << "\n";
        }
    }

    return false;
}

void RISCVMachineInstrHazards::insertNOOPNInstructions(MachineBasicBlock &MBB, const RISCVInstrInfo *TII) {
    SmallSet<unsigned, 32> liveRegs;
    SmallSet<unsigned, 32> defRegs;
    unsigned count = 0;
    DebugLoc DL;

    MachineBasicBlock::iterator InsertPos = MBB.end();

    for (auto MI = MBB.begin(); MI != MBB.end(); ++MI) {
        bool isIndependent = true;

        // Check for register dependencies
        if (hasDataHazard(*MI, liveRegs, defRegs)) {
            isIndependent = false;
        }

        // Check for memory dependencies with previous instructions
        for (auto MII = MBB.begin(); MII != MI; ++MII) {
            if (hasMemoryDependency(*MI, *MII)) {
                isIndependent = false;
                break;
            }
        }

        if (isIndependent) {
            if (count == 0) {
                InsertPos = MI; // Mark the position before the first independent instruction
            }
            count++;
        } else {
            if (count > 0) {
                // Insert NOOPN instruction before the sequence of independent instructions
                BuildMI(MBB, InsertPos, DL, TII->get(RISCV::NOOPN), RISCV::X28)
                    .addReg(RISCV::X28)
                    .addImm(count);
                count = 0;
                liveRegs.clear();
                defRegs.clear();
            }
        }

        // Update live and def registers
        updateLiveAndDefRegs(*MI, liveRegs, defRegs);
    }

    // Handle remaining independent instructions at the end of the basic block
    if (count > 0) {
        BuildMI(MBB, InsertPos, DL, TII->get(RISCV::NOOPN), RISCV::X28)
            .addReg(RISCV::X28)
            .addImm(count);
    }
}

bool RISCVMachineInstrHazards::hasDataHazard(const MachineInstr &MI, const SmallSet<unsigned, 32> &liveRegs, const SmallSet<unsigned, 32> &defRegs) {
    for (const MachineOperand &MO : MI.operands()) {
        if (MO.isReg()) {
            unsigned Reg = MO.getReg();
            if (Reg == RISCV::NoRegister) continue;
            // Read After Write (RAW) hazard
            if (MO.isUse() && defRegs.count(Reg)) {
                return true;
            }
            // Write After Read (WAR) hazard
            if (MO.isDef() && liveRegs.count(Reg)) {
                return true;
            }
            // Write After Write (WAW) hazard
            if (MO.isDef() && defRegs.count(Reg)) {
                return true;
            }
        }
    }
    return false;
}

bool RISCVMachineInstrHazards::hasMemoryDependency(const MachineInstr &MI1, const MachineInstr &MI2) {
    if (!MI1.mayLoadOrStore() || !MI2.mayLoadOrStore())
        return false;

    // Check for potential memory dependencies
    if (MI1.mayStore() || MI2.mayStore()) {
        for (const MachineOperand &MO1 : MI1.operands()) {
            if (MO1.isReg() && MO1.isUse()) {
                for (const MachineOperand &MO2 : MI2.operands()) {
                    if (MO2.isReg() && MO2.isUse() && MO1.getReg() == MO2.getReg()) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

void RISCVMachineInstrHazards::updateLiveAndDefRegs(const MachineInstr &MI, SmallSet<unsigned, 32> &liveRegs, SmallSet<unsigned, 32> &defRegs) {
    for (const MachineOperand &MO : MI.operands()) {
        if (MO.isReg()) {
            unsigned Reg = MO.getReg();
            if (Reg == RISCV::NoRegister) continue;
            if (MO.isDef()) {
                defRegs.insert(Reg);
                liveRegs.erase(Reg); // Remove from liveRegs if it's being redefined
            } else if (MO.isUse()) {
                liveRegs.insert(Reg);
            }
        }
    }
}

} // end of anonymous namespace

INITIALIZE_PASS(RISCVMachineInstrHazards, "riscv-machineinstr-hazards",
    "RISC-V Machine Instruction Hazards Pass",
    true, // is CFG only?
    true  // is analysis?
)

namespace llvm {

FunctionPass *createRISCVMachineInstrHazards() { return new RISCVMachineInstrHazards(); }

} // end of llvm namespace
