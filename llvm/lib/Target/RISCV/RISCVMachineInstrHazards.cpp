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
    bool RAW_hazard(const MachineInstr &Cur_MI, const MachineInstr &MI2, const SmallSet<unsigned, 32> &liveRegs, const SmallSet<unsigned, 32> &defRegs);
};

char RISCVMachineInstrHazards::ID = 0;

bool RISCVMachineInstrHazards::runOnMachineFunction(MachineFunction &MF) {
    // Print the name of the function being processed
    errs() << "Running RISC-V Machine Instruction Hazards Pass on function: " << MF.getName() << "\n";

// Get the instruction information for the current subtarget
    const RISCVSubtarget &STI = MF.getSubtarget<RISCVSubtarget>();
    const RISCVInstrInfo *TII = STI.getInstrInfo();

// Loop through each basic block in the machine function
    for (MachineBasicBlock &MBB : MF) {
        errs() << "  Processing Machine Basic Block: " << MBB.getName() << "\n";   // Print the name of the basic block being processed
        insertNOOPNInstructions(MBB, TII);    // Insert NOOPN instructions into the basic block

// Loop through each instruction in the basic block and print it
        for (MachineInstr &MI : MBB) {
            errs() << "    x Instruction: ";
            MI.print(errs());
            errs() << "\n";
        }
    }

    return false;
}

void RISCVMachineInstrHazards::insertNOOPNInstructions(MachineBasicBlock &MBB, const RISCVInstrInfo *TII) {
    // Initialize sets for live and defined registers, and a counter for independent instructions

    SmallSet<unsigned, 32> liveRegs;
    SmallSet<unsigned, 32> defRegs;
    unsigned count = 0;
    DebugLoc DL;

    MachineBasicBlock::iterator InsertPos = MBB.end();

    for (auto MI = MBB.begin(); MI != MBB.end(); ++MI) {
        bool isIndependent = true;

        // when I have this instructions I do not want to check for Hazzards because I do not know what the will do
        if (MI->mayLoadOrStore() || MI->isBarrier() || MI->isBranch() || MI->isInlineAsm()) {
            if (count != 0){
                // Insert a NOOPN instruction if there were independent instructions before this    
                BuildMI(MBB, InsertPos, DL, TII->get(RISCV::NOOPN), RISCV::X28)
                        .addReg(RISCV::X28)
                        .addImm(count);
                count = 0;   
            }
            continue;
        }
        
        // if MI == CFI_INSTRUCTION or Debug continue;
        if (MI->isDebugInstr() || MI->isCFIInstruction()) {continue;}

        // Check for RAW hazards with previous instructions
        for (auto MII = MBB.begin(); MII != MI; ++MII) {
            if (RAW_hazard(*MI, *MII, liveRegs, defRegs)) {
                isIndependent = false; // if a hazard found mark the instruction as a dependent
                break;
            }
        }

        if (isIndependent) {
            if (count == 0) {
                InsertPos = MI; // (Update the InsertPos) Mark the position before the first independent instruction
            }
            count++;
        } else { // the is not independent and the count > 0 
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
        // updateLiveAndDefRegs(*MI, liveRegs, defRegs);
    }
}

bool RISCVMachineInstrHazards::hasDataHazard(const MachineInstr &Cur_MI, const SmallSet<unsigned, 32> &liveRegs, const SmallSet<unsigned, 32> &defRegs) {
    // Check for RAW hazards within the current instruction
    for (const MachineOperand &MO : Cur_MI.operands()) {
        if (MO.isReg()) {
            unsigned Reg = MO.getReg();
            if (Reg == RISCV::NoRegister) continue;
            // Read After Write (RAW) hazard
            if (MO.isUse() && defRegs.count(Reg)) {
                return true;
            }
        }
    }
    return false;
}

bool RISCVMachineInstrHazards::RAW_hazard(const MachineInstr &Cur_MI, const MachineInstr &MI2, const SmallSet<unsigned, 32> &liveRegs, const SmallSet<unsigned, 32> &defRegs) {

    // Check for RAW hazards between the current instruction and a previous instruction
    for (const MachineOperand &MO1 : Cur_MI.operands()) {
        if (MO1.isReg() && MO1.isUse() && MO1.getReg()!=RISCV::NoRegister) { /*.isUse true if is used for reading a value from register*/
            for (const MachineOperand &MO2 : MI2.operands()) {
                if (MO2.isReg() && MO1.getReg() == MO2.getReg() && MO2.isDef() && MO2.getReg()!=RISCV::NoRegister) { /*.isDef() true if is used for writing  a value to the register*/
                    return true;
                }
            }
        }
    }

    return false;
}

void RISCVMachineInstrHazards::updateLiveAndDefRegs(const MachineInstr &MI, SmallSet<unsigned, 32> &liveRegs, SmallSet<unsigned, 32> &defRegs) {
    // Update live and defined registers based on the current instruction's operands
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
