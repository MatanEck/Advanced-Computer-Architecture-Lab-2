# Advanced-Computer-Architecture-Lab-2
ASIC hierarchical verification: low level simulator

In this lab, we'll build a low level cycle accurate simulator for the processor, and verify its behavior against the high level ISS.
In contrast to the high level ISS which focuses only on the programmer visible instruction set architecture (ISA), the low
level simulator describes a specific micro architecture implementation. There can be many different micro architecture
implementations of the same processor ISA, each one with different performance and power characteristics, but all
compatible from the instruction set point of view (e.g. pipelined and non-pipelined).
The low level simulator, in contrast to the high level simulator, also keeps track of the exact timing of each command, and
can tell us the state of each register, each cycle. 

**SP micro architecture**
We'll design a low performance, serial (non pipelined) implementation of the processor, consisting of the following micro architecture:

Registers:
==============
r2 – r7: [31:0]: 6 32 bit registers, holding the ISA general purpose registers.
pc: [15:0] 16 bit program counter.
inst: [31:0] 32 bit instruction.
opcode [4:0]: 5 bit opcode.
dst[2:0]: 3 bit destination register index.
src0[2:0]: 3 bit source #0 register index.
src1[2:0]: 3 bit source #1 register index.
immediate[31:0]: sign extended immediate.
alu0[31:0]: 32 bit alu operand #0.
alu1[31:0]: 32 bit alu operand #1.
aluout[31:0]: 32 bit alu output.
cycle_counter[31:0]: 32 bit cycle counter.
ctl_state[2:0]: control logic state machine register.

Memory:
==============
Single synchronous SRAM, 65536 lines x 32 bit each. Each cycle either a read or a write can be performed. A read
operation will return the data next cycle.

ALU:
==============
The ALU operates on two 32-bit operands alu0[31:0] and alu1[31:0], performs the operation specified by the opcode, and
stores the output in aluout[31:0]. For ADD,SUB,LSF,RSF,AND,OR,XOR, it performs the requested operation. For LHI, it
performs (alu1 << 16) | alu0[15:0]. For JLT, JLE, JEQ, JNE, it compares ALU0 and ALU1, returning 1 in aluout if the
compare succeeded, and 0 otherwise. For the rest of the commands, the ALU doesn't do anything.

Control logic:
==============
IDLE (state #0): Waits till start signal is given before processing to FETCH0.
FETCH0 (state #1): Issues read command to memory to fetch the current instruction from address PC.
FETCH1 (state #2): Samples memory output to the inst register.
DEC0 (state #3): Decodes the instruction register into its fields: opcode, dst, src0, src1, and immediate (sign extended).
DEC1 (state #4): Prepares the ALU operands.
EXEC0:(state #5): Executes ALU and LD operations.
EXEC1: (state #6): Write backs ALU and memory (ST). On HLT goes to IDLE, otherwise loops back to FETCH0.


*Part of Advanced-Computer-Architecture-Lab in TAU. Assignment instructions were copied - All rights reserved to Tel Aviv University.
