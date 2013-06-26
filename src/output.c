// ACME - a crossassembler for producing 6502/65c02/65816 code.
// Copyright (C) 1998-2009 Marco Baye
// Have a look at "acme.c" for further info
//
// Output stuff
// 24 Nov 2007	Added possibility to suppress segment overlap warnings 
// 25 Sep 2011	Fixed bug in !to (colons in filename could be interpreted as EOS)
#include <stdlib.h>
//#include <stdio.h>
#include <string.h>	// for memset()
#include "acme.h"
#include "alu.h"
#include "config.h"
#include "cpu.h"
#include "dynabuf.h"
#include "global.h"
#include "input.h"
#include "output.h"
#include "platform.h"
#include "tree.h"


// Structure for linked list of segment data
struct segment_t {
	struct segment_t	*next,
				*prev;
	intval_t		start,
				length;
};


// constants
#define OUTBUFFERSIZE	65536


// variables

// segment stuff
static struct segment_t		segments_head;	// head element of segment list
static intval_t			segment_start;	// start of current segment
static intval_t			segment_max;	// highest address segment may use
static int			segment_flags;	// "overlay" and "invisible" flags
// misc
static intval_t	lowest_idx;	// smallest address program uses
static intval_t	highest_idx;	// end address of program plus one
// output buffer stuff
static char	*output_buffer	= NULL;	// to hold assembled code
static char	*write_ptr	= NULL;	// points into output_buffer
intval_t	write_idx;	// index in output buffer
static int	memory_initialised	= FALSE;
// predefined stuff
static struct node_t	*file_format_tree	= NULL;	// tree to hold output formats
// possible file formats
enum out_format_t {
	OUTPUT_FORMAT_UNSPECIFIED,	// default (uses "plain" actually)
	OUTPUT_FORMAT_APPLE,		// load address, length, code
	OUTPUT_FORMAT_CBM,		// load address, code (default for "!to" pseudo opcode)
	OUTPUT_FORMAT_PLAIN		// code only
};
static struct node_t	file_formats[]	= {
	PREDEFNODE("apple",	OUTPUT_FORMAT_APPLE),
	PREDEFNODE(s_cbm,	OUTPUT_FORMAT_CBM),
//	PREDEFNODE("o65",	OUTPUT_FORMAT_O65),
	PREDEFLAST("plain",	OUTPUT_FORMAT_PLAIN),
	//    ^^^^ this marks the last element
};
// chosen file format
static enum out_format_t	output_format	= OUTPUT_FORMAT_UNSPECIFIED;
// predefined stuff
static struct node_t	*segment_modifier_tree	= NULL;	// tree to hold segment modifiers
// segment modifiers
#define	SEGMENT_FLAG_OVERLAY	(1u << 0)
#define	SEGMENT_FLAG_INVISIBLE	(1u << 1)
static struct node_t	segment_modifiers[]	= {
	PREDEFNODE("overlay",	SEGMENT_FLAG_OVERLAY),
	PREDEFLAST("invisible",	SEGMENT_FLAG_INVISIBLE),
	//    ^^^^ this marks the last element
};


// fill output buffer with given byte value
static void fill_completely(char value)
{
	memset(output_buffer, value, OUTBUFFERSIZE);
}


// set up new segment_max value according to the given address.
// just find the next segment start and subtract 1.
static void find_segment_max(intval_t new_pc)
{
	struct segment_t	*test_segment	= segments_head.next;

	// search for smallest segment start address that
	// is larger than given address
	// use list head as sentinel
	segments_head.start = OUTBUFFERSIZE;
	while (test_segment->start <= new_pc)
		test_segment = test_segment->next;
	segment_max = test_segment->start;
	segment_max--;	// last free address available
}


//
static void border_crossed(int current_offset)
{
	if (current_offset >= OUTBUFFERSIZE)
		Throw_serious_error("Produced too much code.");
	if (pass_count == 0) {
		Throw_warning("Segment reached another one, overwriting it.");
		find_segment_max(current_offset + 1);
	}
}


// send low byte to output buffer
void (*Output_byte)(intval_t byte);
static void real_output(intval_t byte);	// fn for actual output
static void no_output(intval_t byte);	// fn when output impossible


// set output pointer (negative values deactivate output)
static void set_mem_ptr(signed long index)
{
	if (index < 0) {
		Output_byte = no_output;
		write_ptr = output_buffer;
		write_idx = 0;
	} else {
		Output_byte = real_output;
		write_ptr = output_buffer + index;
		write_idx = index;
	}
}


// send low byte to output buffer, automatically increasing program counter
static void real_output(intval_t byte)
{
	if (write_idx > segment_max)
		border_crossed(write_idx);
	*write_ptr++ = byte & 0xff;
	write_idx++;
	CPU_2add++;
}


// fail to write to output buffer
static void no_output(intval_t byte)
{
	Throw_error(exception_pc_undefined);
	// set ptr to not complain again. as we have thrown an error, assembly
	// fails, so don't care about actual value.
	set_mem_ptr(512);	// 512 to not garble zero page and stack. ;)
	CPU_2add++;
}


// call this if really calling Output_byte would be a waste of time
void Output_fake(int size)
{
	// check whether ptr undefined
	if (Output_byte == no_output) {
		no_output(0);
		size--;
	}
	if (write_idx + size - 1 > segment_max)
		border_crossed(write_idx + size - 1);
	write_ptr += size;
	write_idx += size;
	CPU_2add += size;
}


// output 8-bit value with range check
void Output_8b(intval_t value)
{
	if ((value <= 0xff) && (value >= -0x80))
		Output_byte(value);
	else
		Throw_error(exception_number_out_of_range);
}


// output 16-bit value with range check
void Output_16b(intval_t value)
{
	if ((value <= 0xffff) && (value >= -0x8000)) {
		Output_byte(value);
		Output_byte(value >> 8);
	} else {
		Throw_error(exception_number_out_of_range);
	}
}


// output 24-bit value with range check
void Output_24b(intval_t value)
{
	if ((value <= 0xffffff) && (value >= -0x800000)) {
		Output_byte(value);
		Output_byte(value >> 8);
		Output_byte(value >> 16);
	} else {
		Throw_error(exception_number_out_of_range);
	}
}


// output 32-bit value (without range check)
void Output_32b(intval_t value)
{
//  if ((Value <= 0x7fffffff) && (Value >= -0x80000000)) {
	Output_byte(value);
	Output_byte(value >> 8);
	Output_byte(value >> 16);
	Output_byte(value >> 24);
//  } else {
//	Throw_error(exception_number_out_of_range);
//  }
}


// define default value for empty memory ("!initmem" pseudo opcode)
static enum eos_t PO_initmem(void)
{
	intval_t	content;

	// ignore in all passes but in first
	if (pass_count)
		return SKIP_REMAINDER;

	// if MemInit flag is already set, complain
	if (memory_initialised) {
		Throw_warning("Memory already initialised.");
		return SKIP_REMAINDER;
	}
	// set MemInit flag
	memory_initialised = TRUE;
	// get value and init memory
	content = ALU_defined_int();
	if ((content > 0xff) || (content < -0x80))
		Throw_error(exception_number_out_of_range);
	// init memory
	fill_completely(content);
	// enforce another pass
	if (pass_undefined_count == 0)
		pass_undefined_count = 1;
// FIXME - enforcing another pass is not needed if there hasn't been any
// output yet. But that's tricky to detect without too much overhead.
// The old solution was to add &&(lowest_idx < highest_idx) to "if" above
	return ENSURE_EOS;
}


// try to set output format held in DynaBuf. Returns whether succeeded.
int Output_set_output_format(void)
{
	void	*node_body;

	if (!Tree_easy_scan(file_format_tree, &node_body, GlobalDynaBuf))
		return FALSE;

	output_format = (enum out_format_t) node_body;
	return TRUE;
}


// select output file and format ("!to" pseudo opcode)
static enum eos_t PO_to(void)
{
	// bugfix: first read filename, *then* check for first pass.
	// if skipping right away, quoted colons might be misinterpreted as EOS
	// FIXME - why not just fix the skipping code to handle quotes? :)
	// "!sl" has been fixed as well

	// read filename to global dynamic buffer
	// if no file name given, exit (complaining will have been done)
	if (Input_read_filename(FALSE))
		return SKIP_REMAINDER;

	// only act upon this pseudo opcode in first pass
	if (pass_count)
		return SKIP_REMAINDER;

	// if output file already chosen, complain and exit
	if (output_filename) {
		Throw_warning("Output file already chosen.");
		return SKIP_REMAINDER;
	}

	// get malloc'd copy of filename
	output_filename = DynaBuf_get_copy(GlobalDynaBuf);
	// select output format
	// if no comma found, use default file format
	if (Input_accept_comma() == FALSE) {
		if (output_format == OUTPUT_FORMAT_UNSPECIFIED) {
			output_format = OUTPUT_FORMAT_CBM;
			// output deprecation warning
			Throw_warning("Used \"!to\" without file format indicator. Defaulting to \"cbm\".");
		}
		return ENSURE_EOS;
	}

	// parse output format name
	// if no keyword given, give up
	if (Input_read_and_lower_keyword() == 0)
		return SKIP_REMAINDER;

	if (Output_set_output_format())
		return ENSURE_EOS;	// success

	// error occurred
	Throw_error("Unknown output format.");
	return SKIP_REMAINDER;
}


// pseudo ocpode table
static struct node_t	pseudo_opcodes[]	= {
	PREDEFNODE("initmem",	PO_initmem),
	PREDEFLAST("to",	PO_to),
	//    ^^^^ this marks the last element
};


// init file format tree (is done early)
void Outputfile_init(void)
{
	Tree_add_table(&file_format_tree, file_formats);
}


// alloc and init mem buffer, register pseudo opcodes (done later)
void Output_init(signed long fill_value)
{
	output_buffer = safe_malloc(OUTBUFFERSIZE);
	write_ptr = output_buffer;
	if (fill_value == MEMINIT_USE_DEFAULT)
		fill_value = FILLVALUE_INITIAL;
	else
		memory_initialised = TRUE;
	// init output buffer (fill memory with initial value)
	fill_completely(fill_value & 0xff);
	Tree_add_table(&pseudo_opcode_tree, pseudo_opcodes);
	Tree_add_table(&segment_modifier_tree, segment_modifiers);
	// init ring list of segments
	segments_head.next = &segments_head;
	segments_head.prev = &segments_head;
}


// dump memory buffer into output file
void Output_save_file(FILE *fd)
{
	intval_t	amount	= highest_idx - lowest_idx;

	if (Process_verbosity)
		printf("Saving %ld ($%lx) bytes ($%lx - $%lx exclusive).\n",
			amount, amount, lowest_idx, highest_idx);
	// output file header according to file format
	switch (output_format) {
	case OUTPUT_FORMAT_APPLE:
		PLATFORM_SETFILETYPE_APPLE(output_filename);
		// output 16-bit load address in little-endian byte order
		putc(lowest_idx & 255, fd);
		putc(lowest_idx >>  8, fd);
		// output 16-bit length in little-endian byte order
		putc(amount & 255, fd);
		putc(amount >>  8, fd);
		break;
	case OUTPUT_FORMAT_UNSPECIFIED:
	case OUTPUT_FORMAT_PLAIN:
		PLATFORM_SETFILETYPE_PLAIN(output_filename);
		break;
	case OUTPUT_FORMAT_CBM:
		PLATFORM_SETFILETYPE_CBM(output_filename);
		// output 16-bit load address in little-endian byte order
		putc(lowest_idx & 255, fd);
		putc(lowest_idx >>  8, fd);
	}
	// dump output buffer to file
	fwrite(output_buffer + lowest_idx, sizeof(char), amount, fd);
}


// link segment data into segment ring
static void link_segment(intval_t start, intval_t length)
{
	struct segment_t	*new_segment,
				*test_segment	= segments_head.next;

	// init new segment
	new_segment = safe_malloc(sizeof(*new_segment));
	new_segment->start = start;
	new_segment->length = length;
	// use ring head as sentinel
	segments_head.start = start;
	segments_head.length = length + 1;
	// walk ring to find correct spot
	while ((test_segment->start < new_segment->start) ||
		((test_segment->start == new_segment->start) &&
		(test_segment->length < new_segment->length)))
		test_segment = test_segment->next;
	// link into ring
	new_segment->next = test_segment;
	new_segment->prev = test_segment->prev;
	new_segment->next->prev = new_segment;
	new_segment->prev->next = new_segment;
}


// show start and end of current segment
// called whenever a new segment begins, and at end of pass.
void Output_end_segment(void)
{
	intval_t	amount;

	if (CPU_uses_pseudo_pc())
		Throw_first_pass_warning("Offset assembly still active at end of segment.");	// FIXME - should be error!
	if ((pass_count == 0) && !(segment_flags & SEGMENT_FLAG_INVISIBLE)) {
		amount = write_idx - segment_start;
		link_segment(segment_start, amount);
		if (Process_verbosity > 1)
			printf(
"Segment size is %ld ($%lx) bytes ($%lx - $%lx exclusive).\n",
amount, amount, segment_start, write_idx);
	}

// FIXME - this was in real_output():
	// check for new max address (FIXME - move to close_segment?)
	if (write_idx > highest_idx)
		highest_idx = write_idx;
}


// check whether given PC is inside segment.
// only call in first pass, otherwise too many warnings might be thrown
static void check_segment(intval_t new_pc)
{
	struct segment_t	*test_segment	= segments_head.next;

	// use list head as sentinel
	segments_head.start = new_pc + 1;
	segments_head.length = 1;
	// search ring for matching entry
	while (test_segment->start <= new_pc) {
		if ((test_segment->start + test_segment->length) > new_pc) {
			Throw_warning("Segment starts inside another one, overwriting it.");
			return;
		}

		test_segment = test_segment->next;
	}
}


// init lowest and highest address
static void init_borders(intval_t address)
{
	lowest_idx = address;
	highest_idx = address;
}


// clear segment list
void Output_passinit(signed long start_addr)
{
//	struct segment_t	*temp;
//FIXME - why clear ring list in every pass?
	// delete segment list (and free blocks)
//	while ((temp = segment_list)) {
//		segment_list = segment_list->next;
//		free(temp);
//	}
	set_mem_ptr(start_addr);	// negative values deactivate output
	// if start address given, set program counter
	if (start_addr >= 0) {
		CPU_set_pc(start_addr);
		// FIXME - integrate next two?
		init_borders(start_addr);
		segment_start = start_addr;
	} else {
		init_borders(0);	// set to _something_ (for !initmem)
		segment_start = 0;
	}
	// other stuff
	segment_max = OUTBUFFERSIZE - 1;
	segment_flags = 0;
}


// called when "*=EXPRESSION" is parsed
void Output_start_segment(void)
{
	void		*node_body;
	int		new_flags	= 0;
	intval_t	new_addr	= ALU_defined_int();

	// check for modifiers
	while (Input_accept_comma()) {
		// parse modifier
		// if no keyword given, give up
		if (Input_read_and_lower_keyword() == 0)
			return;

		if (!Tree_easy_scan(segment_modifier_tree, &node_body, GlobalDynaBuf)) {
			Throw_error("Unknown \"*=\" segment modifier.");
			return;
		}

		new_flags |= (int) node_body;
	}

// to allow for undefined pseudopc, this must be changed to use memaddress instead!
	if (CPU_pc.flags & MVALUE_DEFINED) {
		// it's a redefinition. Check some things:
		// check whether new low
		if (new_addr < lowest_idx)
			lowest_idx = new_addr;
		// show status of previous segment
		Output_end_segment();
		// in first pass, maybe issue warning
		if (pass_count == 0) {
			if (!(new_flags & SEGMENT_FLAG_OVERLAY))
				check_segment(new_addr);
			find_segment_max(new_addr);
		}
	} else {
		init_borders(new_addr);	// it's the first pc definition
	}
	CPU_set_pc(new_addr);
	segment_start = new_addr;
	segment_flags = new_flags;
	set_mem_ptr(new_addr);
}
