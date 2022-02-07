/***************************************************************

	Submitted by:
	Matan Eckhaus Moyal 312161284
	Bar Kachlon 207630864

****************************************************************/
#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>

#include "llsim.h"

#define sp_printf(a...)						\
	do {							\
		llsim_printf("sp: clock %d: ", llsim->clock);	\
		llsim_printf(a);				\
	} while (0)

int nr_simulated_instructions = 0;
FILE *inst_trace_fp = NULL, *cycle_trace_fp = NULL;

typedef struct sp_registers_s {
	// 6 32 bit registers (r[0], r[1] don't exist)
	int r[8];

	// 16 bit program counter
	int pc;

	// 32 bit instruction
	int inst;

	// 5 bit opcode
	int opcode;

	// 3 bit destination register index
	int dst;

	// 3 bit source #0 register index
	int src0;

	// 3 bit source #1 register index
	int src1;

	// 32 bit alu #0 operand
	int alu0;

	// 32 bit alu #1 operand
	int alu1;

	// 32 bit alu output
	int aluout;

	// 32 bit immediate field (original 16 bit sign extended)
	int immediate;

	// 32 bit cycle counter
	int cycle_counter;

	// 3 bit control state machine state register
	int ctl_state;

	// 2 bit DMA state machine
	int dma_state;

	// 32 bit DMA source address register
	int dma_src;

	// 32 bit DMA destination register
	int dma_dst;

	// 32 bit DMA length
	int dma_length;

	// control states
	#define CTL_STATE_IDLE		0
	#define CTL_STATE_FETCH0	1
	#define CTL_STATE_FETCH1	2
	#define CTL_STATE_DEC0		3
	#define CTL_STATE_DEC1		4
	#define CTL_STATE_EXEC0		5
	#define CTL_STATE_EXEC1		6

	// DMA states
	#define DMA_STATE_IDLE		0
	#define DMA_STATE_WAIT		1
	#define DMA_STATE_FETCH		2
	#define DMA_STATE_EXEC		3

} sp_registers_t;

/*
 * Master structure
 */
typedef struct sp_s {
	// local sram
#define SP_SRAM_HEIGHT	64 * 1024
	llsim_memory_t *sram;

	unsigned int memory_image[SP_SRAM_HEIGHT];
	int memory_image_size;

	sp_registers_t *spro, *sprn;
	
	int start;

	int dma_start;  // for initialization of DMA process
	int dma_status; // 0 for busy DMA; 1 for free DMA
	int memory_status; // Memory: 0 for busy; 1 for free

} sp_t;

static void sp_reset(sp_t *sp)
{
	sp_registers_t *sprn = sp->sprn;

	memset(sprn, 0, sizeof(*sprn));
}

/*
 * opcodes
 */
#define ADD 0
#define SUB 1
#define LSF 2
#define RSF 3
#define AND 4
#define OR  5
#define XOR 6
#define LHI 7
#define LD 8
#define ST 9
#define JLT 16
#define JLE 17
#define JEQ 18
#define JNE 19
#define JIN 20
#define HLT 24
#define DMA 25 // start new DMA process
#define POL 26 // poll status of DMA process

static char opcode_name[32][4] = {"ADD", "SUB", "LSF", "RSF", "AND", "OR", "XOR", "LHI",
				 "LD", "ST", "U", "U", "U", "U", "U", "U",
				 "JLT", "JLE", "JEQ", "JNE", "JIN", "U", "U", "U",
				 "HLT", "DMA", "POL", "U", "U", "U", "U", "U"};
void dma_ctl(sp_t* sp, sp_registers_t* spro, sp_registers_t* sprn)
{
	int data;

	switch (spro->dma_state) {
	case DMA_STATE_IDLE:
		if (sp->dma_start) { // if DMA process was started go to next state
			sp->dma_status = 0;
			sprn->dma_state = DMA_STATE_FETCH;
		}
		else
			sp->dma_status = 1;
		break;

	case DMA_STATE_WAIT:
		if (sp->memory_status) // if memory is free go to next state else wait
			sprn->dma_state = DMA_STATE_FETCH;
		else
			sprn->dma_state = DMA_STATE_WAIT;
		break;

	case DMA_STATE_FETCH:
		if (sp->memory_status == 0) // if memory is busy wait
			sprn->dma_state = DMA_STATE_WAIT;
		else {
			llsim_mem_read(sp->sram, spro->dma_src);
			sprn->dma_state = DMA_STATE_EXEC;
		}
		break;

	case DMA_STATE_EXEC:
		// copy data from source address to destination address in memory
		data = llsim_mem_extract_dataout(sp->sram, 31, 0);
		llsim_mem_set_datain(sp->sram, data, 31, 0);
		llsim_mem_write(sp->sram, spro->dma_dst);

		// update DMA registers
		sprn->dma_src = spro->dma_src + 1;
		sprn->dma_dst = spro->dma_dst + 1;
		sprn->dma_length = spro->dma_length - 1;

		// check if DMA was done
		if (sprn->dma_length > 0)
			sprn->dma_state = DMA_STATE_FETCH;
		else {
			sp->dma_start = 0; // DMA process was done
			sprn->dma_state = DMA_STATE_IDLE;
		}
		break;
	}
}
static void dump_sram(sp_t *sp)
{
	FILE *fp;
	int i;

	fp = fopen("sram_out.txt", "w");
	if (fp == NULL) {
                printf("couldn't open file sram_out.txt\n");
                exit(1);
	}
	for (i = 0; i < SP_SRAM_HEIGHT; i++)
		fprintf(fp, "%08x\n", llsim_mem_extract(sp->sram, i, 31, 0));
	fclose(fp);
}



static void sp_ctl(sp_t* sp)
{
	sp_registers_t* spro = sp->spro;
	sp_registers_t* sprn = sp->sprn;
	int i;
	int extracted_value = 0, LD_reg_addr = 0, ST_mem_addr = 0, curr_pc = 0;

	// sp_ctl

	fprintf(cycle_trace_fp, "cycle %d\n", spro->cycle_counter);
	for (i = 2; i <= 7; i++)
		fprintf(cycle_trace_fp, "r%d %08x\n", i, spro->r[i]);
	fprintf(cycle_trace_fp, "pc %08x\n", spro->pc);
	fprintf(cycle_trace_fp, "inst %08x\n", spro->inst);
	fprintf(cycle_trace_fp, "opcode %08x\n", spro->opcode);
	fprintf(cycle_trace_fp, "dst %08x\n", spro->dst);
	fprintf(cycle_trace_fp, "src0 %08x\n", spro->src0);
	fprintf(cycle_trace_fp, "src1 %08x\n", spro->src1);
	fprintf(cycle_trace_fp, "immediate %08x\n", spro->immediate);
	fprintf(cycle_trace_fp, "alu0 %08x\n", spro->alu0);
	fprintf(cycle_trace_fp, "alu1 %08x\n", spro->alu1);
	fprintf(cycle_trace_fp, "aluout %08x\n", spro->aluout);
	fprintf(cycle_trace_fp, "cycle_counter %08x\n", spro->cycle_counter);
	fprintf(cycle_trace_fp, "ctl_state %08x\n\n", spro->ctl_state);

	sprn->cycle_counter = spro->cycle_counter + 1;

	switch (spro->ctl_state) {
	case CTL_STATE_IDLE:
		sprn->pc = 0;
		if (sp->start)
			sprn->ctl_state = CTL_STATE_FETCH0;
		break;

	case CTL_STATE_FETCH0:
		sp->memory_status = 0; // set memory busy for DMA
		dma_ctl(sp, spro, sprn);

		llsim_mem_read(sp->sram, spro->pc);//fetch instruction from address PC.
		sprn->ctl_state = CTL_STATE_FETCH1;//Set next state
		break;

	case CTL_STATE_FETCH1:
		sp->memory_status = 1; // set memory free for DMA
		dma_ctl(sp, spro, sprn);

		sprn->inst = llsim_mem_extract_dataout(sp->sram, 31, 0);//Samples memory output to the inst register 
		sprn->ctl_state = CTL_STATE_DEC0;//Set next state
		break;

	case CTL_STATE_DEC0:
		sp->memory_status = 1; // set emory free for DMA
		dma_ctl(sp, spro, sprn);

		sprn->opcode = (spro->inst >> 25) & 0x1F; //masking and shifting for opcode
		sprn->dst = (spro->inst >> 22) & 0x07;	// masking and shifting for dst
		sprn->src0 = (spro->inst >> 19) & 0x07;	//masking and shifting for src0
		sprn->src1 = (spro->inst >> 16) & 0x07;	// masking and shifting for src1
		sprn->immediate = spro->inst & 0x0000ffff; // masking and sign extension for immediate
		sprn->immediate <<= 16;
		sprn->immediate >>= 16;
		sprn->ctl_state = CTL_STATE_DEC1; //Set next state
		break;

	case CTL_STATE_DEC1:
		sp->memory_status = 1; // set memory free for DMA
		dma_ctl(sp, spro, sprn);

		if (spro->opcode == LHI) {// In case of LHI
			sprn->alu0 = spro->r[spro->dst];
			sprn->alu1 = spro->immediate;
		}
		else {
			//Set alu0
			if (spro->src0 == 0)
				sprn->alu0 = 0;
			else if (spro->src0 == 1)
				sprn->alu0 = spro->immediate;
			else
				sprn->alu0 = spro->r[spro->src0];
			//Set alu1
			if (spro->src1 == 0)
				sprn->alu1 = 0;
			else if (spro->src1 == 1)
				sprn->alu1 = spro->immediate;
			else
				sprn->alu1 = spro->r[spro->src1];
		}
		sprn->ctl_state = CTL_STATE_EXEC0; //Set next state
		break;
	case CTL_STATE_EXEC0:
		sp->memory_status = 0; // set memory as busy for DMA
		dma_ctl(sp, spro, sprn);

		switch (spro->opcode)
		{
		case ADD:
			sprn->aluout = spro->alu0 + spro->alu1;
			break;
		case SUB:
			sprn->aluout = spro->alu0 - spro->alu1;
			break;
		case RSF:
			sprn->aluout = spro->alu0 >> spro->alu1;
			break;
		case LSF:
			sprn->aluout = spro->alu0 << spro->alu1;
			break;
		case OR:
			sprn->aluout = spro->alu0 | spro->alu1;
			break;
		case XOR:
			sprn->aluout = spro->alu0 ^ spro->alu1;
			break;
		case AND:
			sprn->aluout = spro->alu0 & spro->alu1;
			break;
		case LHI:
			sprn->aluout = (spro->alu0 & 0x0000ffff) | (spro->alu1 << 16);
			break;
		case LD:
			llsim_mem_read(sp->sram, spro->alu1);
			break;
		case JLT:
			if (spro->alu0 < spro->alu1)
				sprn->aluout = 1;
			else
				sprn->aluout = 0;
			break;
		case ST:
			break;
		case JLE:
			if (spro->alu0 <= spro->alu1)
				sprn->aluout = 1;
			else
				sprn->aluout = 0;
			break;
		case JNE:
			if (spro->alu0 != spro->alu1)
				sprn->aluout = 1;
			else
				sprn->aluout = 0;
			break;
		case JEQ:
			if (spro->alu0 == spro->alu1)
				sprn->aluout = 1;
			else
				sprn->aluout = 0;
			break;
		case JIN:
			sprn->aluout = 1;
			break;
		case HLT:
			break;
		}
		sprn->ctl_state = CTL_STATE_EXEC1; //Set next state
		break;

	case CTL_STATE_EXEC1:
		sp->memory_status = 0; // set memory as free for DMA
		dma_ctl(sp, spro, sprn);

		sprn->pc = sprn->pc + 1;
		switch (spro->opcode)
		{
		case HLT:
			sp->start = 0;
			dump_sram(sp);
			llsim_stop();
			sprn->ctl_state = CTL_STATE_IDLE;
			break;
		case LD:
			extracted_value = llsim_mem_extract_dataout(sp->sram, 31, 0);
			sprn->r[spro->dst] = extracted_value;
			break;
		case ST:
			llsim_mem_set_datain(sp->sram, spro->alu0, 31, 0);
			llsim_mem_write(sp->sram, spro->alu1);
			break;
		case ADD:
		case SUB:
		case LSF:
		case RSF:
		case AND:
		case OR:
		case XOR:
		case LHI:
			sprn->r[spro->dst] = spro->aluout;
			break;
		case JLT:
		case JLE:
		case JEQ:
		case JNE:
		case JIN:
			if (spro->aluout) {
				sprn->pc = spro->immediate;
				sprn->r[7] = spro->pc; //jump to immediate and save the pc in r[7].
				sprn->ctl_state = CTL_STATE_FETCH0; //Set next state
			}
			break;
		case DMA: //Begin DMA process
			sprn->dma_dst = spro->r[spro->dst];
			sprn->dma_src = spro->alu0;
			sprn->dma_length = spro->alu1;
			break;
		case POL: //POL - Return 0 if DMA busy; 1 if DMA finished
			sprn->r[spro->dst] = sp->dma_status;
			break;
		}
		if (sprn->ctl_state != CTL_STATE_IDLE)
			sprn->ctl_state = CTL_STATE_FETCH0;

		if (spro->opcode == DMA) { // in case of DMA opcode, start DMA process
			sp->dma_start = 1;
			dma_ctl(sp, spro, sprn);

			break;
		}
		//write to inst_trace file
		if (spro->ctl_state == 6) {
			fprintf(inst_trace_fp, "--- instruction %i (%04x) @ PC %i (%04x) -----------------------------------------------------------\n", (spro->cycle_counter) / 6 - 1, (spro->cycle_counter) / 6 - 1, spro->pc, spro->pc);

			fprintf(inst_trace_fp, "pc = %04d, inst = %08x, opcode = %i (%s), dst = %i, src0 = %i, src1 = %i, immediate = %08x\n", spro->pc, spro->inst, spro->opcode, opcode_name[spro->opcode], spro->dst, spro->src0, spro->src1, spro->immediate);
			fprintf(inst_trace_fp, "r[0] = 00000000 r[1] = %08x r[2] = %08x r[3] = %08x \n", spro->immediate, spro->r[2], spro->r[3]);

			fprintf(inst_trace_fp, "r[4] = %08x r[5] = %08x r[6] = %08x r[7] = %08x \n\n", spro->r[4], spro->r[5], spro->r[6], spro->r[7]);

			if (spro->opcode <= LHI) {
				fprintf(inst_trace_fp, ">>>> EXEC: R[%i] = %i %s %i <<<<\n\n", spro->dst, spro->alu0, opcode_name[spro->opcode], spro->alu1);
			}
			else if (spro->opcode == LD)
			{
				if (spro->src1 == 1) {
					LD_reg_addr = spro->immediate;
				}
				else
					LD_reg_addr = spro->r[spro->src1];
				fprintf(inst_trace_fp, ">>>> EXEC: R[%i] = MEM[%i] = %08x <<<<\n\n", spro->dst, LD_reg_addr, extracted_value);
			}
			else if (spro->opcode == ST)
			{
				if (spro->src1 == 1) {
					ST_mem_addr = spro->immediate;
				}
				else
					ST_mem_addr = spro->r[spro->src1];
				fprintf(inst_trace_fp, ">>>> EXEC: MEM[%i] = R[%i] = %08x <<<<\n\n", ST_mem_addr, spro->src0, spro->r[spro->src0]);
			}
			else if (spro->opcode == JLT || spro->opcode == JLE || spro->opcode == JEQ || spro->opcode == JNE || spro->opcode == JIN)
			{
				if (spro->aluout) {
					curr_pc = spro->immediate;
				}
				else
					curr_pc = spro->pc + 1;

				fprintf(inst_trace_fp, ">>>> EXEC: %s %i, %i, %i <<<<\n\n", opcode_name[spro->opcode], spro->r[spro->src0], spro->r[spro->src1], curr_pc);
			}
			else if (spro->opcode == HLT)
			{
				fprintf(inst_trace_fp, ">>>> EXEC: HALT at PC %04x<<<<\n", spro->pc);
				fprintf(inst_trace_fp, "sim finished at pc %i, %i instructions", spro->pc, (spro->cycle_counter) / 6);
			}
			if (spro->opcode == DMA) // DMA copy instruction
				fprintf(inst_trace_fp, ">>>> EXEC: DMA %d %d %d <<<<\n\n", sprn->dma_dst, sprn->dma_src, sprn->dma_length);
			if (spro->opcode == POL) // DMA poll instruction
				fprintf(inst_trace_fp, ">>>> EXEC: POL R[%d]=%d <<<<\n\n", spro->dst, sp->dma_status);
		}
	}
}



static void sp_run(llsim_unit_t *unit)
{
	sp_t *sp = (sp_t *) unit->private;

	if (llsim->reset) {
		sp_reset(sp);
		return;
	}

	sp->sram->read = 0;
	sp->sram->write = 0;

	sp_ctl(sp);
}

static void sp_generate_sram_memory_image(sp_t *sp, char *program_name)
{
        FILE *fp;
        int addr, i;

        fp = fopen(program_name, "r");
        if (fp == NULL) {
                printf("couldn't open file %s\n", program_name);
                exit(1);
        }
        addr = 0;
        while (addr < SP_SRAM_HEIGHT) {
                fscanf(fp, "%08x\n", &sp->memory_image[addr]);
                addr++;
                if (feof(fp))
                        break;
        }
	sp->memory_image_size = addr;

        fprintf(inst_trace_fp, "program %s loaded, %d lines\n\n", program_name, addr);

	for (i = 0; i < sp->memory_image_size; i++)
		llsim_mem_inject(sp->sram, i, sp->memory_image[i], 31, 0);
}

static void sp_register_all_registers(sp_t *sp)
{
	sp_registers_t *spro = sp->spro, *sprn = sp->sprn;

	// registers
	llsim_register_register("sp", "r_0", 32, 0, &spro->r[0], &sprn->r[0]);
	llsim_register_register("sp", "r_1", 32, 0, &spro->r[1], &sprn->r[1]);
	llsim_register_register("sp", "r_2", 32, 0, &spro->r[2], &sprn->r[2]);
	llsim_register_register("sp", "r_3", 32, 0, &spro->r[3], &sprn->r[3]);
	llsim_register_register("sp", "r_4", 32, 0, &spro->r[4], &sprn->r[4]);
	llsim_register_register("sp", "r_5", 32, 0, &spro->r[5], &sprn->r[5]);
	llsim_register_register("sp", "r_6", 32, 0, &spro->r[6], &sprn->r[6]);
	llsim_register_register("sp", "r_7", 32, 0, &spro->r[7], &sprn->r[7]);

	llsim_register_register("sp", "pc", 16, 0, &spro->pc, &sprn->pc);
	llsim_register_register("sp", "inst", 32, 0, &spro->inst, &sprn->inst);
	llsim_register_register("sp", "opcode", 5, 0, &spro->opcode, &sprn->opcode);
	llsim_register_register("sp", "dst", 3, 0, &spro->dst, &sprn->dst);
	llsim_register_register("sp", "src0", 3, 0, &spro->src0, &sprn->src0);
	llsim_register_register("sp", "src1", 3, 0, &spro->src1, &sprn->src1);
	llsim_register_register("sp", "alu0", 32, 0, &spro->alu0, &sprn->alu0);
	llsim_register_register("sp", "alu1", 32, 0, &spro->alu1, &sprn->alu1);
	llsim_register_register("sp", "aluout", 32, 0, &spro->aluout, &sprn->aluout);
	llsim_register_register("sp", "immediate", 32, 0, &spro->immediate, &sprn->immediate);
	llsim_register_register("sp", "cycle_counter", 32, 0, &spro->cycle_counter, &sprn->cycle_counter);
	llsim_register_register("sp", "ctl_state", 3, 0, &spro->ctl_state, &sprn->ctl_state);
}

void sp_init(char *program_name)
{
	llsim_unit_t *llsim_sp_unit;
	llsim_unit_registers_t *llsim_ur;
	sp_t *sp;

	llsim_printf("initializing sp unit\n");

	inst_trace_fp = fopen("inst_trace.txt", "w");
	if (inst_trace_fp == NULL) {
		printf("couldn't open file inst_trace.txt\n");
		exit(1);
	}

	cycle_trace_fp = fopen("cycle_trace.txt", "w");
	if (cycle_trace_fp == NULL) {
		printf("couldn't open file cycle_trace.txt\n");
		exit(1);
	}

	llsim_sp_unit = llsim_register_unit("sp", sp_run);
	llsim_ur = llsim_allocate_registers(llsim_sp_unit, "sp_registers", sizeof(sp_registers_t));
	sp = llsim_malloc(sizeof(sp_t));
	llsim_sp_unit->private = sp;
	sp->spro = llsim_ur->old;
	sp->sprn = llsim_ur->new;

	sp->sram = llsim_allocate_memory(llsim_sp_unit, "sram", 32, SP_SRAM_HEIGHT, 0);
	sp_generate_sram_memory_image(sp, program_name);

	sp->start = 1;

	sp_register_all_registers(sp);
}
