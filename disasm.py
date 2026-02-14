import struct
import sys
data = open('SCPH1001.BIN', 'rb').read()
start = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x220
end = int(sys.argv[2], 16) if len(sys.argv) > 2 else start + 0x80
for i in range(start, min(end, len(data)), 4):
    inst = struct.unpack('<I', data[i:i+4])[0]
    op = (inst >> 26) & 0x3F
    rs = (inst >> 21) & 0x1F
    rt = (inst >> 16) & 0x1F
    rd = (inst >> 11) & 0x1F
    imm = inst & 0xFFFF
    simm = imm if imm < 0x8000 else imm - 0x10000
    func = inst & 0x3F
    sa = (inst >> 6) & 0x1F
    rn = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
          't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
          's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
          't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
    desc = '%08X' % inst
    if inst == 0:
        desc += '  nop'
    elif op == 0x0F:
        desc += '  lui %s, 0x%04X' % (rn[rt], imm)
    elif op == 0x0D:
        desc += '  ori %s, %s, 0x%04X' % (rn[rt], rn[rs], imm)
    elif op == 0x09:
        desc += '  addiu %s, %s, %d' % (rn[rt], rn[rs], simm)
    elif op == 0x0C:
        desc += '  andi %s, %s, 0x%04X' % (rn[rt], rn[rs], imm)
    elif op == 0x10:
        if rs == 4:
            desc += '  mtc0 %s, COP0[%d]' % (rn[rt], rd)
        elif rs == 0:
            desc += '  mfc0 %s, COP0[%d]' % (rn[rt], rd)
        elif rs == 0x10 and func == 0x10:
            desc += '  rfe'
    elif op == 0x2B:
        desc += '  sw %s, %d(%s)' % (rn[rt], simm, rn[rs])
    elif op == 0x23:
        desc += '  lw %s, %d(%s)' % (rn[rt], simm, rn[rs])
    elif op == 0x00:
        if func == 0x25:
            desc += '  or %s, %s, %s' % (rn[rd], rn[rs], rn[rt])
        elif func == 0x08:
            desc += '  jr %s' % rn[rs]
        elif func == 0x00 and sa > 0:
            desc += '  sll %s, %s, %d' % (rn[rd], rn[rt], sa)
        elif func == 0x02:
            desc += '  srl %s, %s, %d' % (rn[rd], rn[rt], sa)
        elif func == 0x21:
            desc += '  addu %s, %s, %s' % (rn[rd], rn[rs], rn[rt])
        elif func == 0x24:
            desc += '  and %s, %s, %s' % (rn[rd], rn[rs], rn[rt])
    elif op == 0x04:
        desc += '  beq %s, %s, 0x%X' % (rn[rs], rn[rt], (i+4+simm*4) & 0xFFFFF)
    elif op == 0x05:
        desc += '  bne %s, %s, 0x%X' % (rn[rs], rn[rt], (i+4+simm*4) & 0xFFFFF)
    elif op == 0x02:
        desc += '  j 0x%08X' % ((inst & 0x3FFFFFF) << 2)
    elif op == 0x03:
        desc += '  jal 0x%08X' % ((inst & 0x3FFFFFF) << 2)
    elif op == 0x20:
        desc += '  lb %s, %d(%s)' % (rn[rt], simm, rn[rs])
    elif op == 0x24:
        desc += '  lbu %s, %d(%s)' % (rn[rt], simm, rn[rs])
    elif op == 0x28:
        desc += '  sb %s, %d(%s)' % (rn[rt], simm, rn[rs])
    elif op == 0x29:
        desc += '  sh %s, %d(%s)' % (rn[rt], simm, rn[rs])
    print('BFC%05X: %s' % (i, desc))
