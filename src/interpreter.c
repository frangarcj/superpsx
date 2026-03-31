#include "superpsx.h"
#include "interpreter.h"
#include "scheduler.h"
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>

/* R3000A Instruction Opcodes */
#define OP_SPECIAL 0x00
#define OP_REGIMM  0x01
#define OP_J       0x02
#define OP_JAL     0x03
#define OP_BEQ     0x04
#define OP_BNE     0x05
#define OP_BLEZ    0x06
#define OP_BGTZ    0x07
#define OP_ADDI    0x08
#define OP_ADDIU   0x09
#define OP_SLTI    0x0A
#define OP_SLTIU   0x0B
#define OP_ANDI    0x0C
#define OP_ORI     0x0D
#define OP_XORI    0x0E
#define OP_LUI     0x0F
#define OP_COP0    0x10
#define OP_COP1    0x11
#define OP_COP2    0x12
#define OP_COP3    0x13
#define OP_LB      0x20
#define OP_LH      0x21
#define OP_LWL     0x22
#define OP_LW      0x23
#define OP_LBU     0x24
#define OP_LHU     0x25
#define OP_LWR     0x26
#define OP_SB      0x28
#define OP_SH      0x29
#define OP_SWL     0x2A
#define OP_SW      0x2B
#define OP_SWR     0x2E
#define OP_LWC0    0x30
#define OP_LWC1    0x31
#define OP_LWC2    0x32
#define OP_LWC3    0x33
#define OP_SWC0    0x38
#define OP_SWC1    0x39
#define OP_SWC2    0x3A
#define OP_SWC3    0x3B

/* SPECIAL Functs */
#define FUNCT_SLL  0x00
#define FUNCT_SRL  0x02
#define FUNCT_SRA  0x03
#define FUNCT_SLLV 0x04
#define FUNCT_SRLV 0x06
#define FUNCT_SRAV 0x07
#define FUNCT_JR   0x08
#define FUNCT_JALR 0x09
#define FUNCT_SYSCALL 0x0C
#define FUNCT_BREAK 0x0D
#define FUNCT_MFHI 0x10
#define FUNCT_MTHI 0x11
#define FUNCT_MFLO 0x12
#define FUNCT_MTLO 0x13
#define FUNCT_MULT 0x18
#define FUNCT_MULTU 0x19
#define FUNCT_DIV  0x1A
#define FUNCT_DIVU 0x1B
#define FUNCT_ADD  0x20
#define FUNCT_ADDU 0x21
#define FUNCT_SUB  0x22
#define FUNCT_SUBU 0x23
#define FUNCT_AND  0x24
#define FUNCT_OR   0x25
#define FUNCT_XOR  0x26
#define FUNCT_NOR  0x27
#define FUNCT_SLT  0x2A
#define FUNCT_SLTU 0x2B

/* REGIMM rt values */
#define RT_BLTZ    0x00
#define RT_BGEZ    0x01
#define RT_BLTZAL  0x10
#define RT_BGEZAL  0x11

static inline int instruction_is_load(uint32_t opcode) {
    uint32_t op = opcode >> 26;
    return (op >= 0x20 && op <= 0x26);
}

static inline int instruction_writes_gpr(uint32_t opcode, int reg) {
    if (reg == 0) return 0;
    uint32_t op = opcode >> 26;
    if (op == 0x00) {
        uint32_t func = opcode & 0x3F;
        if (func >= 0x18 && func <= 0x1B) return 0; /* MULT/DIV */
        if (func == 0x08) return 0; /* JR */
        if (func == 0x11 || func == 0x13) return 0; /* MTHI/MTLO */
        if (func == 0x0C || func == 0x0D) return 0; /* SYSCALL/BREAK */
        return (((opcode >> 11) & 0x1F) == reg); /* RD */
    }
    if (op >= 0x08 && op <= 0x0F) return (((opcode >> 16) & 0x1F) == reg);
    if (op >= 0x20 && op <= 0x26) return (((opcode >> 16) & 0x1F) == reg);
    if (op == 0x03) return reg == 31;
    if (op == 0x00 && (opcode & 0x3F) == 0x09) return (((opcode >> 11) & 0x1F) == reg);
    if (op == 0x12) { /* COP2 */
        if (((opcode >> 21) & 0x1F) == 0x00 || ((opcode >> 21) & 0x1F) == 0x02) { /* MFC2 / CFC2 */
            return (((opcode >> 16) & 0x1F) == reg);
        }
    }
    if (op == 0x10) { /* COP0 */
        if (((opcode >> 21) & 0x1F) == 0x00) { /* MFC0 */
            return (((opcode >> 16) & 0x1F) == reg);
        }
    }
    return 0;
}

static uint32_t branch_target = 0;
static int branch_state = 0; /* 0=no branch, 1=in delay slot, 2=execute branch */

static void do_branch(uint32_t target) {
    branch_target = target;
    branch_state = 1;
}

void interpreter_reset_state(void) {
    branch_state = 0;
    branch_target = 0;
}

int run_interpreter_chain(uint64_t deadline) {
    /* branch_state/branch_target are NOT reset on entry — the function
     * may be re-entered after a deadline-triggered return.  However,
     * we guarantee we NEVER return with branch_state != 0: the outer
     * loop's sync_hardware_and_interrupts() could fire an exception,
     * and a stale branch_state would corrupt the ISR by committing
     * the old branch target after the first ISR instruction. */

    extern int binary_loaded;

    while (global_cycles < deadline || branch_state != 0) {
        /* BIOS boot hook: the outer loop only checks between calls,
         * but the interpreter runs many instructions per call.
         * Check the idle-loop PCs so Phase 1 can detect them. */
        if (__builtin_expect(!binary_loaded && (
                cpu.pc == 0x80030000 ||
                (cpu.pc >= 0x001A45A0 && cpu.pc <= 0x001A4620)), 0))
            return 0;

        /* BIOS HLE hooks: intercept PSX BIOS call vectors A(xx), B(xx), C(xx).
         * The JIT injects these at compile time; the interpreter must do it at
         * runtime.  The functions are called with cpu.regs[9] = function number.
         * Return value 1 = handled (abort and return to caller). */
        {
            uint32_t phys = cpu.pc & 0x1FFFFFFF;
            if (__builtin_expect(phys == 0xA0 || phys == 0xB0 || phys == 0xC0, 0)) {
                extern int BIOS_HLE_A(void);
                extern int BIOS_HLE_B(void);
                extern int BIOS_HLE_C(void);
                int handled = 0;
                if (phys == 0xA0)      handled = BIOS_HLE_A();
                else if (phys == 0xB0) handled = BIOS_HLE_B();
                else                   handled = BIOS_HLE_C();
                if (handled) {
                    /* HLE handled it — return to caller via $ra */
                    cpu.pc = cpu.regs[31];
                    global_cycles += 10;
                    continue;
                }
            }
        }

        if (__builtin_expect(cpu.pc & 3, 0)) {
            cpu.cop0[PSX_COP0_BADVADDR] = cpu.pc;
            cpu.pc = cpu.current_pc;
            PSX_Exception(4); /* AdEL */
            branch_state = 0;
            return 0;
        }

        cpu.current_pc = cpu.pc;
        uint32_t opcode = ReadWord(cpu.pc);
        
        cpu.pc += 4; /* Advance PC */
        
        /* Apply load delay pipeline */
        if (cpu.load_commit_reg) {
            /* If a new load targets the same register, the older commit is cancelled */
            if (cpu.load_delay_reg != cpu.load_commit_reg) {
                cpu.regs[cpu.load_commit_reg] = cpu.load_commit_val;
                cpu.regs[0] = 0;
            }
            cpu.load_commit_reg = 0;
        }
        if (cpu.load_delay_reg) {
            cpu.load_commit_reg = cpu.load_delay_reg;
            cpu.load_commit_val = cpu.load_delay_val;
            cpu.load_delay_reg = 0;
        }
        
        uint32_t op = opcode >> 26;
        uint32_t rs = (opcode >> 21) & 31;
        uint32_t rt = (opcode >> 16) & 31;
        uint32_t rd = (opcode >> 11) & 31;
        uint32_t shamt = (opcode >> 6) & 31;
        uint32_t funct = opcode & 63;
        uint16_t imm = opcode & 0xFFFF;
        int32_t imm_se = (int32_t)(int16_t)imm;
        uint32_t target = opcode & 0x3FFFFFF;

        /* Execute instruction */
        switch (op) {
            case OP_SPECIAL:
                switch (funct) {
                    case FUNCT_SLL:  if (rd) cpu.regs[rd] = cpu.regs[rt] << shamt; break;
                    case FUNCT_SRL:  if (rd) cpu.regs[rd] = cpu.regs[rt] >> shamt; break;
                    case FUNCT_SRA:  if (rd) cpu.regs[rd] = (int32_t)cpu.regs[rt] >> shamt; break;
                    case FUNCT_SLLV: if (rd) cpu.regs[rd] = cpu.regs[rt] << (cpu.regs[rs] & 31); break;
                    case FUNCT_SRLV: if (rd) cpu.regs[rd] = cpu.regs[rt] >> (cpu.regs[rs] & 31); break;
                    case FUNCT_SRAV: if (rd) cpu.regs[rd] = (int32_t)cpu.regs[rt] >> (cpu.regs[rs] & 31); break;
                    case FUNCT_JR:   do_branch(cpu.regs[rs]); break;
                    case FUNCT_JALR: {
                        uint32_t ret = cpu.pc + 4;
                        do_branch(cpu.regs[rs]);
                        if (rd) cpu.regs[rd] = ret;
                        break;
                    }
                    case FUNCT_SYSCALL: Helper_Syscall_Exception(cpu.current_pc); branch_state = 0; return 0;
                    case FUNCT_BREAK:   Helper_Break_Exception(cpu.current_pc); branch_state = 0; return 0;
                    case FUNCT_MFHI: if (rd) cpu.regs[rd] = cpu.hi; break;
                    case FUNCT_MTHI: cpu.hi = cpu.regs[rs]; break;
                    case FUNCT_MFLO: if (rd) cpu.regs[rd] = cpu.lo; break;
                    case FUNCT_MTLO: cpu.lo = cpu.regs[rs]; break;
                    case FUNCT_MULT: {
                        int64_t val = (int64_t)(int32_t)cpu.regs[rs] * (int64_t)(int32_t)cpu.regs[rt];
                        cpu.hi = (uint32_t)(val >> 32);
                        cpu.lo = (uint32_t)val;
                        break;
                    }
                    case FUNCT_MULTU: {
                        uint64_t val = (uint64_t)cpu.regs[rs] * (uint64_t)cpu.regs[rt];
                        cpu.hi = (uint32_t)(val >> 32);
                        cpu.lo = (uint32_t)val;
                        break;
                    }
                    case FUNCT_DIV:  Helper_DIV(cpu.regs[rs], cpu.regs[rt], &cpu.lo, &cpu.hi); break;
                    case FUNCT_DIVU: Helper_DIVU(cpu.regs[rs], cpu.regs[rt], &cpu.lo, &cpu.hi); break;
                    case FUNCT_ADD:  Helper_ADD(cpu.regs[rs], cpu.regs[rt], rd, cpu.current_pc); break;
                    case FUNCT_ADDU: if (rd) cpu.regs[rd] = cpu.regs[rs] + cpu.regs[rt]; break;
                    case FUNCT_SUB:  Helper_SUB(cpu.regs[rs], cpu.regs[rt], rd, cpu.current_pc); break;
                    case FUNCT_SUBU: if (rd) cpu.regs[rd] = cpu.regs[rs] - cpu.regs[rt]; break;
                    case FUNCT_AND:  if (rd) cpu.regs[rd] = cpu.regs[rs] & cpu.regs[rt]; break;
                    case FUNCT_OR:   if (rd) cpu.regs[rd] = cpu.regs[rs] | cpu.regs[rt]; break;
                    case FUNCT_XOR:  if (rd) cpu.regs[rd] = cpu.regs[rs] ^ cpu.regs[rt]; break;
                    case FUNCT_NOR:  if (rd) cpu.regs[rd] = ~(cpu.regs[rs] | cpu.regs[rt]); break;
                    case FUNCT_SLT:  if (rd) cpu.regs[rd] = ((int32_t)cpu.regs[rs] < (int32_t)cpu.regs[rt]) ? 1 : 0; break;
                    case FUNCT_SLTU: if (rd) cpu.regs[rd] = (cpu.regs[rs] < cpu.regs[rt]) ? 1 : 0; break;
                    default:
                        Helper_Break_Exception(cpu.current_pc); /* Invalid instruction */
                        branch_state = 0;
                        return 0;
                }
                break;
            case OP_REGIMM:
                switch (rt) {
                    case RT_BLTZ:   if ((int32_t)cpu.regs[rs] < 0) do_branch(cpu.pc + (imm_se << 2)); break;
                    case RT_BGEZ:   if ((int32_t)cpu.regs[rs] >= 0) do_branch(cpu.pc + (imm_se << 2)); break;
                    case RT_BLTZAL: {
                        uint32_t ret = cpu.pc + 4;
                        if ((int32_t)cpu.regs[rs] < 0) do_branch(cpu.pc + (imm_se << 2));
                        cpu.regs[31] = ret;
                        break;
                    }
                    case RT_BGEZAL: {
                        uint32_t ret = cpu.pc + 4;
                        if ((int32_t)cpu.regs[rs] >= 0) do_branch(cpu.pc + (imm_se << 2));
                        cpu.regs[31] = ret;
                        break;
                    }
                    default: {
                        /* R3000A undocumented: only bit[0] matters for direction.
                         * Link only for exactly 0x10/0x11 (handled above). */
                        if (rt & 1) {
                            if ((int32_t)cpu.regs[rs] >= 0) do_branch(cpu.pc + (imm_se << 2));
                        } else {
                            if ((int32_t)cpu.regs[rs] < 0) do_branch(cpu.pc + (imm_se << 2));
                        }
                        break;
                    }
                }
                break;
            case OP_J:   do_branch((cpu.pc & 0xF0000000) | (target << 2)); break;
            case OP_JAL: {
                uint32_t ret = cpu.pc + 4;
                do_branch((cpu.pc & 0xF0000000) | (target << 2));
                cpu.regs[31] = ret;
                break;
            }
            case OP_BEQ:  if (cpu.regs[rs] == cpu.regs[rt]) do_branch(cpu.pc + (imm_se << 2)); break;
            case OP_BNE:  if (cpu.regs[rs] != cpu.regs[rt]) do_branch(cpu.pc + (imm_se << 2)); break;
            case OP_BLEZ: if ((int32_t)cpu.regs[rs] <= 0) do_branch(cpu.pc + (imm_se << 2)); break;
            case OP_BGTZ: if ((int32_t)cpu.regs[rs] > 0) do_branch(cpu.pc + (imm_se << 2)); break;
            case OP_ADDI: Helper_ADDI(cpu.regs[rs], imm_se, rt, cpu.current_pc); break;
            case OP_ADDIU: if (rt) cpu.regs[rt] = cpu.regs[rs] + imm_se; break;
            case OP_SLTI:  if (rt) cpu.regs[rt] = ((int32_t)cpu.regs[rs] < imm_se) ? 1 : 0; break;
            case OP_SLTIU: if (rt) cpu.regs[rt] = (cpu.regs[rs] < (uint32_t)imm_se) ? 1 : 0; break;
            case OP_ANDI:  if (rt) cpu.regs[rt] = cpu.regs[rs] & imm; break;
            case OP_ORI:   if (rt) cpu.regs[rt] = cpu.regs[rs] | imm; break;
            case OP_XORI:  if (rt) cpu.regs[rt] = cpu.regs[rs] ^ imm; break;
            case OP_LUI:   if (rt) cpu.regs[rt] = imm << 16; break;
            case OP_COP0:
                switch (rs) {
                    case 0x00: /* MFC0 */
                        if (rt) {
                            cpu.load_delay_reg = rt;
                            cpu.load_delay_val = cpu.cop0[rd];
                        }
                        break;
                    case 0x04: /* MTC0 */
                        cpu.cop0[rd] = cpu.regs[rt];
                        break;
                    case 0x10: /* RFE */ {
                        uint32_t sr = cpu.cop0[PSX_COP0_SR];
                        uint32_t mode = sr & 0x3F;
                        uint32_t new_mode = ((mode >> 2) & 0x0F) | (mode & 0x30);
                        cpu.cop0[PSX_COP0_SR] = (sr & ~0x3F) | new_mode;
                        break;
                    }
                }
                break;
            case OP_COP1: /* Unused */ Helper_CU_Exception(cpu.current_pc, 1); branch_state = 0; return 0;
            case OP_COP2: /* GTE */
                if (rs == 0x00) { /* MFC2 */
                    if (rt) { cpu.regs[rt] = GTE_ReadData(&cpu, rd); }
                } else if (rs == 0x02) { /* CFC2 */
                    if (rt) { cpu.regs[rt] = GTE_ReadCtrl(&cpu, rd); }
                } else if (rs == 0x04) { /* MTC2 */
                    GTE_WriteData(&cpu, rd, cpu.regs[rt]);
                } else if (rs == 0x06) { /* CTC2 */
                    GTE_WriteCtrl(&cpu, rd, cpu.regs[rt]);
                } else {
                    GTE_Execute(opcode, &cpu);
                }
                break;
            case OP_COP3: /* Unused */ Helper_CU_Exception(cpu.current_pc, 3); branch_state = 0; return 0;
            case OP_LB:  if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = (int32_t)(int8_t)ReadByte(cpu.regs[rs] + imm_se); } break;
            case OP_LH: {
                uint32_t addr = cpu.regs[rs] + imm_se;
                if (__builtin_expect(addr & 1, 0)) { cpu.cop0[PSX_COP0_BADVADDR] = addr; cpu.pc = cpu.current_pc; PSX_Exception(4); branch_state = 0; return 0; }
                if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = (int32_t)(int16_t)ReadHalf(addr); }
                break;
            }
            case OP_LWL: {
                uint32_t base_val = cpu.regs[rt];
                /* Pipeline forward: use pending commit value if targeting same reg */
                if (cpu.load_commit_reg == (uint32_t)rt && rt != 0)
                    base_val = cpu.load_commit_val;
                if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = Helper_LWL(cpu.regs[rs] + imm_se, base_val); }
                break;
            }
            case OP_LW: {
                uint32_t addr = cpu.regs[rs] + imm_se;
                if (__builtin_expect(addr & 3, 0)) { cpu.cop0[PSX_COP0_BADVADDR] = addr; cpu.pc = cpu.current_pc; PSX_Exception(4); branch_state = 0; return 0; }
                if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = ReadWord(addr); }
                break;
            }
            case OP_LBU: if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = ReadByte(cpu.regs[rs] + imm_se); } break;
            case OP_LHU: {
                uint32_t addr = cpu.regs[rs] + imm_se;
                if (__builtin_expect(addr & 1, 0)) { cpu.cop0[PSX_COP0_BADVADDR] = addr; cpu.pc = cpu.current_pc; PSX_Exception(4); branch_state = 0; return 0; }
                if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = ReadHalf(addr); }
                break;
            }
            case OP_LWR: {
                uint32_t base_val = cpu.regs[rt];
                /* Pipeline forward: use pending commit value if targeting same reg */
                if (cpu.load_commit_reg == (uint32_t)rt && rt != 0)
                    base_val = cpu.load_commit_val;
                if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = Helper_LWR(cpu.regs[rs] + imm_se, base_val); }
                break;
            }
            case OP_SB:  WriteByte(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            case OP_SH: {
                uint32_t addr = cpu.regs[rs] + imm_se;
                if (__builtin_expect(addr & 1, 0)) { cpu.cop0[PSX_COP0_BADVADDR] = addr; cpu.pc = cpu.current_pc; PSX_Exception(5); branch_state = 0; return 0; }
                WriteHalf(addr, cpu.regs[rt]);
                break;
            }
            case OP_SWL: Helper_SWL(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            case OP_SW: {
                uint32_t addr = cpu.regs[rs] + imm_se;
                if (__builtin_expect(addr & 3, 0)) { cpu.cop0[PSX_COP0_BADVADDR] = addr; cpu.pc = cpu.current_pc; PSX_Exception(5); branch_state = 0; return 0; }
                WriteWord(addr, cpu.regs[rt]);
                break;
            }
            case OP_SWR: Helper_SWR(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            /* LWC/SWC mappings for COP0/COP2 are not valid in PSX except for GTE data, but standard doesn't use LWC2 usually.
             * But for safety, intercept COP exceptions. */
            case OP_LWC0: Helper_CU_Exception(cpu.current_pc, 0); branch_state = 0; return 0;
            case OP_LWC1: Helper_CU_Exception(cpu.current_pc, 1); branch_state = 0; return 0;
            case OP_LWC2: { /* Load Word to COP2 (GTE data register) */
                uint32_t addr = cpu.regs[rs] + imm_se;
                if (__builtin_expect(addr & 3, 0)) { cpu.cop0[PSX_COP0_BADVADDR] = addr; cpu.pc = cpu.current_pc; PSX_Exception(4); branch_state = 0; return 0; }
                uint32_t val = ReadWord(addr);
                GTE_WriteData(&cpu, rt, val);
                break;
            }
            case OP_LWC3: Helper_CU_Exception(cpu.current_pc, 3); branch_state = 0; return 0;
            case OP_SWC0: Helper_CU_Exception(cpu.current_pc, 0); branch_state = 0; return 0;
            case OP_SWC1: Helper_CU_Exception(cpu.current_pc, 1); branch_state = 0; return 0;
            case OP_SWC2: { /* Store Word from COP2 (GTE data register) */
                uint32_t addr = cpu.regs[rs] + imm_se;
                if (__builtin_expect(addr & 3, 0)) { cpu.cop0[PSX_COP0_BADVADDR] = addr; cpu.pc = cpu.current_pc; PSX_Exception(5); branch_state = 0; return 0; }
                uint32_t val = GTE_ReadData(&cpu, rt);
                WriteWord(addr, val);
                break;
            }
            case OP_SWC3: Helper_CU_Exception(cpu.current_pc, 3); branch_state = 0; return 0;
            default:
                Helper_Break_Exception(cpu.current_pc);
                branch_state = 0;
                return 0;
        }

        /* Cancel delayed load if this instruction overwrites its destination.
         * Note: hardware suppresses the load entirely if the overwriting
         * instruction is NOT a load itself. */
        if (cpu.load_commit_reg && !instruction_is_load(opcode) &&
            instruction_writes_gpr(opcode, cpu.load_commit_reg)) {
            cpu.load_commit_reg = 0;
        }

        /* Branches are executed after the delay slot (this instruction) */
        if (branch_state == 2) {
            cpu.pc = branch_target;
            branch_state = 0;
        } else if (branch_state == 1) {
            branch_state = 2;
        }

        { extern uint32_t r3000a_cycle_cost(uint32_t);
          global_cycles += r3000a_cycle_cost(opcode); }

        /* Interrupt check is NOT done here.  The Phase-2 outer loop's
         * sync_hardware_and_interrupts() handles interrupt delivery at
         * deadline boundaries — the same cadence as the DRC.  Checking
         * inside the interpreter loop causes an "interrupt storm" where
         * the ISR monopolises the CPU and the BIOS main loop never
         * advances its state machine. */

        /* If we hit block abort (exception inside branch delay), exit */
        if (__builtin_expect(cpu.block_aborted, 0)) {
            cpu.pc = psx_abort_pc;
            cpu.block_aborted = 0;
            branch_state = 0;
            return 0;
        }

        if (global_cycles >= sched_cached_earliest && branch_state == 0) {
            return 0;
        }
    }
    return 0; /* RUN_RES_NORMAL — guaranteed branch_state == 0 */
}
