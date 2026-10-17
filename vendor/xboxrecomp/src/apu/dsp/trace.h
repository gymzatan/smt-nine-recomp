#pragma once
/* The standalone target does not enable QEMU diagnostic trace events. */
#define trace_dsp_read_peripheral(...) ((void)0)
#define trace_dsp_write_peripheral(...) ((void)0)
#define trace_dsp56k_execute_instruction(...) ((void)0)
#define trace_dsp56k_execute_instruction_disasm(...) ((void)0)
#define trace_event_get_state(...) 0
