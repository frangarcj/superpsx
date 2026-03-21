#include "superpsx.h"
#include "interpreter.h"
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

static uint32_t branch_target = 0;
static int branch_state = 0; /* 0=no branch, 1=in delay slot, 2=execute branch */

static void do_branch(uint32_t target) {
    branch_target = target;
    branch_state = 1;
}

int run_interpreter_chain(uint64_t deadline) {
    if (global_cycles == 0) {
        branch_state = 0;
        branch_target = 0;
        cpu.load_delay_reg = 0;
    }

    while (global_cycles < deadline) {
        if (cpu.pc & 3) {
            cpu.cop0[PSX_COP0_BADVADDR] = cpu.pc;
            cpu.pc = cpu.current_pc;
            PSX_Exception(4); /* AdEL */
            return 0;
        }

        cpu.current_pc = cpu.pc;
        uint32_t opcode = ReadWord(cpu.pc);
        
        cpu.pc += 4; /* Advance PC */
        
        /* Apply load delay from previous instruction */
        uint32_t current_load_reg = cpu.load_delay_reg;
        uint32_t current_load_val = cpu.load_delay_val;
        cpu.load_delay_reg = 0;
        
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
                    case FUNCT_SYSCALL: Helper_Syscall_Exception(cpu.current_pc); return 0;
                    case FUNCT_BREAK:   Helper_Break_Exception(cpu.current_pc); return 0;
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
                    case 0x00: /* MFC0 */ if (rt) cpu.load_delay_reg = rt; cpu.load_delay_val = cpu.cop0[rd]; break;
                    case 0x04: /* MTC0 */ cpu.cop0[rd] = cpu.regs[rt]; break;
                    case 0x10: /* RFE */ {
                        uint32_t mode = cpu.cop0[PSX_COP0_SR] & 0x3F;
                        cpu.cop0[PSX_COP0_SR] = (cpu.cop0[PSX_COP0_SR] & ~0x3F) | (mode >> 2);
                        break;
                    }
                }
                break;
            case OP_COP1: /* Unused */ Helper_CU_Exception(cpu.current_pc, 1); return 0;
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
            case OP_COP3: /* Unused */ Helper_CU_Exception(cpu.current_pc, 3); return 0;
            case OP_LB:  if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = (int32_t)(int8_t)ReadByte(cpu.regs[rs] + imm_se); } break;
            case OP_LH:  if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = (int32_t)(int16_t)ReadHalf(cpu.regs[rs] + imm_se); } break;
            case OP_LWL: if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = Helper_LWL(cpu.regs[rs] + imm_se, cpu.regs[rt]); } break;
            case OP_LW:  if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = ReadWord(cpu.regs[rs] + imm_se); } break;
            case OP_LBU: if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = ReadByte(cpu.regs[rs] + imm_se); } break;
            case OP_LHU: if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = ReadHalf(cpu.regs[rs] + imm_se); } break;
            case OP_LWR: if (rt) { cpu.load_delay_reg = rt; cpu.load_delay_val = Helper_LWR(cpu.regs[rs] + imm_se, cpu.regs[rt]); } break;
            case OP_SB:  WriteByte(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            case OP_SH:  WriteHalf(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            case OP_SWL: Helper_SWL(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            case OP_SW:  WriteWord(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            case OP_SWR: Helper_SWR(cpu.regs[rs] + imm_se, cpu.regs[rt]); break;
            /* LWC/SWC mappings for COP0/COP2 are not valid in PSX except for GTE data, but standard doesn't use LWC2 usually.
             * But for safety, intercept COP exceptions. */
            case OP_LWC0: Helper_CU_Exception(cpu.current_pc, 0); return 0;
            case OP_LWC1: Helper_CU_Exception(cpu.current_pc, 1); return 0;
            case OP_LWC2: Helper_CU_Exception(cpu.current_pc, 2); return 0;
            case OP_LWC3: Helper_CU_Exception(cpu.current_pc, 3); return 0;
            case OP_SWC0: Helper_CU_Exception(cpu.current_pc, 0); return 0;
            case OP_SWC1: Helper_CU_Exception(cpu.current_pc, 1); return 0;
            case OP_SWC2: Helper_CU_Exception(cpu.current_pc, 2); return 0;
            case OP_SWC3: Helper_CU_Exception(cpu.current_pc, 3); return 0;
            default:
                Helper_Break_Exception(cpu.current_pc);
                return 0;
        }

        /* Commit delayed load from the cycle BEFORE this instruction */
        if (current_load_reg) {
            cpu.regs[current_load_reg] = current_load_val;
        }
        cpu.regs[0] = 0;

        /* Branches are executed after the delay slot (this instruction) */
        if (branch_state == 2) {
            cpu.pc = branch_target;
            branch_state = 0;
        } else if (branch_state == 1) {
            branch_state = 2;
        }

        global_cycles += 2; /* 2 cycles approx per inst? The JIT uses variable cycles. Let's use 2. */

        /* If we hit block abort (exception inside branch delay), exit */
        if (cpu.block_aborted) {
            cpu.pc = psx_abort_pc;
            cpu.block_aborted = 0;
            return 0;
        }
    }
    return 0; /* RUN_RES_NORMAL */
}
