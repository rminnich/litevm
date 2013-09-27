#ifndef __LINUX_LITEVM_H
#define __LINUX_LITEVM_H

#include <asm/types.h>
#include <linux/ioctl.h>

/* for LITEVM_CREATE_MEMORY_REGION */
struct litevm_memory_region {
	__u32 slot;
	__u32 flags;
	__u64 guest_phys_addr;
	__u64 memory_size; /* bytes */
};

/* for litevm_memory_region::flags */
#define LITEVM_MEM_LOG_DIRTY_PAGES  1UL


#define LITEVM_EXIT_TYPE_FAIL_ENTRY 1
#define LITEVM_EXIT_TYPE_VM_EXIT    2

enum litevm_exit_reason {
	LITEVM_EXIT_UNKNOWN,
	LITEVM_EXIT_EXCEPTION,
	LITEVM_EXIT_IO,
	LITEVM_EXIT_CPUID,
	LITEVM_EXIT_DEBUG,
	LITEVM_EXIT_HLT,
	LITEVM_EXIT_MMIO,
};

/* for LITEVM_RUN */
struct litevm_run {
	/* in */
	__u32 vcpu;
	__u32 emulated;  /* skip current instruction */
	__u32 mmio_completed; /* mmio request completed */

	/* out */
	__u32 exit_type;
	__u32 exit_reason;
	__u32 instruction_length;
	union {
		/* LITEVM_EXIT_UNKNOWN */
		struct {
			__u32 hardware_exit_reason;
		} hw;
		/* LITEVM_EXIT_EXCEPTION */
		struct {
			__u32 exception;
			__u32 error_code;
		} ex;
		/* LITEVM_EXIT_IO */
		struct {
#define LITEVM_EXIT_IO_IN  0
#define LITEVM_EXIT_IO_OUT 1
			__u8 direction;
			__u8 size; /* bytes */
			__u8 string;
			__u8 string_down;
			__u8 rep;
			__u8 pad;
			__u16 port;
			__u64 count;
			union {
				__u64 address;
				__u32 value;
			};
		} io;
		struct {
		} debug;
		/* LITEVM_EXIT_MMIO */
		struct {
			__u64 phys_addr;
			__u8  data[8];
			__u32 len;
			__u8  is_write;
		} mmio;
	};
};

/* for LITEVM_GET_REGS and LITEVM_SET_REGS */
struct litevm_regs {
	/* in */
	__u32 vcpu;
	__u32 padding;

	/* out (LITEVM_GET_REGS) / in (LITEVM_SET_REGS) */
	__u64 rax, rbx, rcx, rdx;
	__u64 rsi, rdi, rsp, rbp;
	__u64 r8,  r9,  r10, r11;
	__u64 r12, r13, r14, r15;
	__u64 rip, rflags;
};

struct litevm_segment {
	__u64 base;
	__u32 limit;
	__u16 selector;
	__u8  type;
	__u8  present, dpl, db, s, l, g, avl;
	__u8  unusable;
	__u8  padding;
};

struct litevm_dtable {
	__u64 base;
	__u16 limit;
	__u16 padding[3];
};

/* for LITEVM_GET_SREGS and LITEVM_SET_SREGS */
struct litevm_sregs {
	/* in */
	__u32 vcpu;
	__u32 padding;

	/* out (LITEVM_GET_SREGS) / in (LITEVM_SET_SREGS) */
	struct litevm_segment cs, ds, es, fs, gs, ss;
	struct litevm_segment tr, ldt;
	struct litevm_dtable gdt, idt;
	__u64 cr0, cr2, cr3, cr4, cr8;
	__u64 efer;
	__u64 apic_base;

	/* out (LITEVM_GET_SREGS) */
	__u32 pending_int;
	__u32 padding2;
};

/* for LITEVM_TRANSLATE */
struct litevm_translation {
	/* in */
	__u64 linear_address;
	__u32 vcpu;
	__u32 padding;

	/* out */
	__u64 physical_address;
	__u8  valid;
	__u8  writeable;
	__u8  usermode;
};

/* for LITEVM_INTERRUPT */
struct litevm_interrupt {
	/* in */
	__u32 vcpu;
	__u32 irq;
};

struct litevm_breakpoint {
	__u32 enabled;
	__u32 padding;
	__u64 address;
};

/* for LITEVM_DEBUG_GUEST */
struct litevm_debug_guest {
	/* int */
	__u32 vcpu;
	__u32 enabled;
	struct litevm_breakpoint breakpoints[4];
	__u32 singlestep;
};

/* for LITEVM_GET_DIRTY_LOG */
struct litevm_dirty_log {
	__u32 slot;
	__u32 padding;
	union {
		void __user *dirty_bitmap; /* one bit per page */
		__u64 padding;
	};
};

#define LITEVMIO 0xAE

#define LITEVM_RUN                   _IOWR(LITEVMIO, 2, struct litevm_run)
#define LITEVM_GET_REGS              _IOWR(LITEVMIO, 3, struct litevm_regs)
#define LITEVM_SET_REGS              _IOW(LITEVMIO, 4, struct litevm_regs)
#define LITEVM_GET_SREGS             _IOWR(LITEVMIO, 5, struct litevm_sregs)
#define LITEVM_SET_SREGS             _IOW(LITEVMIO, 6, struct litevm_sregs)
#define LITEVM_TRANSLATE             _IOWR(LITEVMIO, 7, struct litevm_translation)
#define LITEVM_INTERRUPT             _IOW(LITEVMIO, 8, struct litevm_interrupt)
#define LITEVM_DEBUG_GUEST           _IOW(LITEVMIO, 9, struct litevm_debug_guest)
#define LITEVM_SET_MEMORY_REGION     _IOW(LITEVMIO, 10, struct litevm_memory_region)
#define LITEVM_CREATE_VCPU           _IOW(LITEVMIO, 11, int /* vcpu_slot */)
#define LITEVM_GET_DIRTY_LOG         _IOW(LITEVMIO, 12, struct litevm_dirty_log)

#endif
