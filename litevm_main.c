/*
 * Kernel-based Virtual Machine driver for Linux
 *
 * This module enables machines with Intel VT-x extensions to run virtual
 * machines without emulation or binary translation.
 *
 * Copyright (C) 2006 Qumranet, Inc.
 *
 * Authors:
 *   Avi Kivity   <avi@qumranet.com>
 *   Yaniv Kamay  <yaniv@qumranet.com>
 *
 */

#include "litevm.h"

#include <linux/litevm.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <asm/processor.h>
#include <linux/percpu.h>
#include <linux/gfp.h>
#include <asm/msr.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>
#include <linux/reboot.h>
#include <asm/io.h>
#include <linux/debugfs.h>
#include <linux/highmem.h>
#include <linux/file.h>

#include "vmx.h"
#include "x86_emulate.h"

MODULE_AUTHOR("Qumranet");
MODULE_LICENSE("GPL");

struct litevm_stat litevm_stat;

static struct litevm_stats_debugfs_item {
	const char *name;
	u32 *data;
	struct dentry *dentry;
} debugfs_entries[] = {
	{ "pf_fixed", &litevm_stat.pf_fixed },
	{ "pf_guest", &litevm_stat.pf_guest },
	{ "tlb_flush", &litevm_stat.tlb_flush },
	{ "invlpg", &litevm_stat.invlpg },
	{ "exits", &litevm_stat.exits },
	{ "io_exits", &litevm_stat.io_exits },
	{ "mmio_exits", &litevm_stat.mmio_exits },
	{ "signal_exits", &litevm_stat.signal_exits },
	{ "irq_exits", &litevm_stat.irq_exits },
	{ 0, 0 }
};

static struct dentry *debugfs_dir;

static const u32 vmx_msr_index[] = {
#ifdef __x86_64__
	MSR_SYSCALL_MASK, MSR_LSTAR, MSR_CSTAR, MSR_KERNEL_GS_BASE,
#endif
	MSR_EFER, // wtf? MSR_K6_STAR,
};
#define NR_VMX_MSR (sizeof(vmx_msr_index) / sizeof(*vmx_msr_index))

#ifdef __x86_64__
/*
 * avoid save/load MSR_SYSCALL_MASK and MSR_LSTAR by std vt
 * mechanism (cpu bug AA24)
 */
#define NR_BAD_MSRS 2
#else
#define NR_BAD_MSRS 0
#endif

#define TSS_IOPB_BASE_OFFSET 0x66
#define TSS_BASE_SIZE 0x68
#define TSS_IOPB_SIZE (65536 / 8)
#define TSS_REDIRECTION_SIZE (256 / 8)
#define RMODE_TSS_SIZE (TSS_BASE_SIZE + TSS_REDIRECTION_SIZE + TSS_IOPB_SIZE + 1)

#define MSR_IA32_VMX_BASIC_MSR   		0x480
#define MSR_IA32_VMX_PINBASED_CTLS_MSR		0x481
#define MSR_IA32_VMX_PROCBASED_CTLS_MSR		0x482
#define MSR_IA32_VMX_EXIT_CTLS_MSR		0x483
#define MSR_IA32_VMX_ENTRY_CTLS_MSR		0x484

#define CR0_RESEVED_BITS 0xffffffff1ffaffc0ULL
#define LMSW_GUEST_MASK 0x0eULL
#define CR4_RESEVED_BITS (~((1ULL << 11) - 1))
#define CR4_VMXE 0x2000
#define CR8_RESEVED_BITS (~0x0fULL)
#define EFER_RESERVED_BITS 0xfffffffffffff2fe

#ifdef __x86_64__
#define HOST_IS_64 1
#else
#define HOST_IS_64 0
#endif

static struct vmx_msr_entry *find_msr_entry(struct litevm_vcpu *vcpu, u32 msr)
{
	int i;

	for (i = 0; i < vcpu->nmsrs; ++i)
		if (vcpu->guest_msrs[i].index == msr)
			return &vcpu->guest_msrs[i];
	return 0;
}

struct descriptor_table {
	u16 limit;
	unsigned long base;
} __attribute__((packed));

static void get_gdt(struct descriptor_table *table)
{
	asm ("sgdt %0" : "=m"(*table));
}

static void get_idt(struct descriptor_table *table)
{
	asm ("sidt %0" : "=m"(*table));
}

static u16 read_fs(void)
{
	u16 seg;
	asm ("mov %%fs, %0" : "=g"(seg));
	return seg;
}

static u16 read_gs(void)
{
	u16 seg;
	asm ("mov %%gs, %0" : "=g"(seg));
	return seg;
}

static u16 read_ldt(void)
{
	u16 ldt;
	asm ("sldt %0" : "=g"(ldt));
	return ldt;
}

static void load_fs(u16 sel)
{
	asm ("mov %0, %%fs" : : "g"(sel));
}

static void load_gs(u16 sel)
{
	asm ("mov %0, %%gs" : : "g"(sel));
}

#ifndef load_ldt
static void load_ldt(u16 sel)
{
	asm ("lldt %0" : : "g"(sel));
}
#endif

static void fx_save(void *image)
{
	asm ("fxsave (%0)":: "r" (image));
}

static void fx_restore(void *image)
{
	asm ("fxrstor (%0)":: "r" (image));
}

static void fpu_init(void)
{
	asm ("finit");
}

struct segment_descriptor {
	u16 limit_low;
	u16 base_low;
	u8  base_mid;
	u8  type : 4;
	u8  system : 1;
	u8  dpl : 2;
	u8  present : 1;
	u8  limit_high : 4;
	u8  avl : 1;
	u8  long_mode : 1;
	u8  default_op : 1;
	u8  granularity : 1;
	u8  base_high;
} __attribute__((packed));

#ifdef __x86_64__
// LDT or TSS descriptor in the GDT. 16 bytes.
struct segment_descriptor_64 {
	struct segment_descriptor s;
	u32 base_higher;
	u32 pad_zero;
};

#endif

static unsigned long segment_base(u16 selector)
{
	struct descriptor_table gdt;
	struct segment_descriptor *d;
	unsigned long table_base;
	typedef unsigned long ul;
	unsigned long v;

	asm ("sgdt %0" : "=m"(gdt));
	table_base = gdt.base;

	if (selector & 4) {           /* from ldt */
		u16 ldt_selector;

		asm ("sldt %0" : "=g"(ldt_selector));
		table_base = segment_base(ldt_selector);
	}
	d = (struct segment_descriptor *)(table_base + (selector & ~7));
	v = d->base_low | ((ul)d->base_mid << 16) | ((ul)d->base_high << 24);
#ifdef __x86_64__
	if (d->system == 0
	    && (d->type == 2 || d->type == 9 || d->type == 11))
		v |= ((ul)((struct segment_descriptor_64 *)d)->base_higher) << 32;
#endif
	return v;
}

static unsigned long read_tr_base(void)
{
	u16 tr;
	asm ("str %0" : "=g"(tr));
	return segment_base(tr);
}

static void reload_tss(void)
{
#ifndef __x86_64__

	/*
	 * VT restores TR but not its size.  Useless.
	 */
	struct descriptor_table gdt;
	struct segment_descriptor *descs;

	get_gdt(&gdt);
	descs = (void *)gdt.base;
	descs[GDT_ENTRY_TSS].type = 9; /* available TSS */
	load_TR_desc();
#endif
}

static DEFINE_PER_CPU(struct vmcs *, vmxarea);
static DEFINE_PER_CPU(struct vmcs *, current_vmcs);

static struct vmcs_descriptor {
	int size;
	int order;
	u32 revision_id;
} vmcs_descriptor;

#ifdef __x86_64__
static unsigned long read_msr(unsigned long msr)
{
	u64 value;

	rdmsrl(msr, value);
	return value;
}
#endif

static inline struct page *_gfn_to_page(struct litevm *litevm, gfn_t gfn)
{
	struct litevm_memory_slot *slot = gfn_to_memslot(litevm, gfn);
	return (slot) ? slot->phys_mem[gfn - slot->base_gfn] : 0;
}



int litevm_read_guest(struct litevm_vcpu *vcpu,
			     gva_t addr,
			     unsigned long size,
			     void *dest)
{
	unsigned char *host_buf = dest;
	unsigned long req_size = size;

	while (size) {
		hpa_t paddr;
		unsigned now;
		unsigned offset;
		hva_t guest_buf;

		paddr = gva_to_hpa(vcpu, addr);

		if (is_error_hpa(paddr))
			break;

		guest_buf = (hva_t)kmap_atomic(
					pfn_to_page(paddr >> PAGE_SHIFT),
					KM_USER0);
		offset = addr & ~PAGE_MASK;
		guest_buf |= offset;
		now = min(size, PAGE_SIZE - offset);
		memcpy(host_buf, (void*)guest_buf, now);
		host_buf += now;
		addr += now;
		size -= now;
		kunmap_atomic((void *)(guest_buf & PAGE_MASK), KM_USER0);
	}
	return req_size - size;
}

int litevm_write_guest(struct litevm_vcpu *vcpu,
			     gva_t addr,
			     unsigned long size,
			     void *data)
{
	unsigned char *host_buf = data;
	unsigned long req_size = size;

	while (size) {
		hpa_t paddr;
		unsigned now;
		unsigned offset;
		hva_t guest_buf;

		paddr = gva_to_hpa(vcpu, addr);

		if (is_error_hpa(paddr))
			break;

		guest_buf = (hva_t)kmap_atomic(
				pfn_to_page(paddr >> PAGE_SHIFT), KM_USER0);
		offset = addr & ~PAGE_MASK;
		guest_buf |= offset;
		now = min(size, PAGE_SIZE - offset);
		memcpy((void*)guest_buf, host_buf, now);
		host_buf += now;
		addr += now;
		size -= now;
		kunmap_atomic((void *)(guest_buf & PAGE_MASK), KM_USER0);
	}
	return req_size - size;
}

static __init void setup_vmcs_descriptor(void)
{
	u32 vmx_msr_low, vmx_msr_high;

	rdmsr(MSR_IA32_VMX_BASIC_MSR, vmx_msr_low, vmx_msr_high);
	vmcs_descriptor.size = vmx_msr_high & 0x1fff;
	vmcs_descriptor.order = get_order(vmcs_descriptor.size);
	vmcs_descriptor.revision_id = vmx_msr_low;
};

static void vmcs_clear(struct vmcs *vmcs)
{
	u64 phys_addr = __pa(vmcs);
	u8 error;

	asm volatile ("vmclear %1; setna %0"
		       : "=m"(error) : "m"(phys_addr) : "cc", "memory" );
	if (error)
		printk(KERN_ERR "litevm: vmclear fail: %p/%llx\n",
		       vmcs, phys_addr);
}

static void __vcpu_clear(void *arg)
{
	struct litevm_vcpu *vcpu = arg;
	int cpu = smp_processor_id();

	if (vcpu->cpu == cpu)
		vmcs_clear(vcpu->vmcs);
	if (per_cpu(current_vmcs, cpu) == vcpu->vmcs)
		per_cpu(current_vmcs, cpu) = 0;
}

static int vcpu_slot(struct litevm_vcpu *vcpu)
{
	return vcpu - vcpu->litevm->vcpus;
}

/*
 * Switches to specified vcpu, until a matching vcpu_put(), but assumes
 * vcpu mutex is already taken.
 */
static struct litevm_vcpu *__vcpu_load(struct litevm_vcpu *vcpu)
{
	u64 phys_addr = __pa(vcpu->vmcs);
	int cpu;

	cpu = get_cpu();

	if (vcpu->cpu != cpu) {
		smp_call_function(__vcpu_clear, vcpu, 0, 1);
		vcpu->launched = 0;
	}

	if (per_cpu(current_vmcs, cpu) != vcpu->vmcs) {
		u8 error;

		per_cpu(current_vmcs, cpu) = vcpu->vmcs;
		asm volatile ("vmptrld %1; setna %0"
			       : "=m"(error) : "m"(phys_addr) : "cc" );
		if (error)
			printk(KERN_ERR "litevm: vmptrld %p/%llx fail\n",
			       vcpu->vmcs, phys_addr);
	}

	if (vcpu->cpu != cpu) {
		struct descriptor_table dt;
		unsigned long sysenter_esp;

		vcpu->cpu = cpu;
		/*
		 * Linux uses per-cpu TSS and GDT, so set these when switching
		 * processors.
		 */
		vmcs_writel(HOST_TR_BASE, read_tr_base()); /* 22.2.4 */
		get_gdt(&dt);
		vmcs_writel(HOST_GDTR_BASE, dt.base);   /* 22.2.4 */

		rdmsrl(MSR_IA32_SYSENTER_ESP, sysenter_esp);
		vmcs_writel(HOST_IA32_SYSENTER_ESP, sysenter_esp); /* 22.2.3 */
	}
	return vcpu;
}

/*
 * Switches to specified vcpu, until a matching vcpu_put()
 */
static struct litevm_vcpu *vcpu_load(struct litevm *litevm, int vcpu_slot)
{
	struct litevm_vcpu *vcpu = &litevm->vcpus[vcpu_slot];

	mutex_lock(&vcpu->mutex);
	if (unlikely(!vcpu->vmcs)) {
		mutex_unlock(&vcpu->mutex);
		return 0;
	}
	return __vcpu_load(vcpu);
}

static void vcpu_put(struct litevm_vcpu *vcpu)
{
	put_cpu();
	mutex_unlock(&vcpu->mutex);
}


static struct vmcs *alloc_vmcs_cpu(int cpu)
{
	int node = cpu_to_node(cpu);
	struct page *pages;
	struct vmcs *vmcs;

	pages = alloc_pages_node(node, GFP_KERNEL, vmcs_descriptor.order);
	if (!pages)
		return 0;
	vmcs = page_address(pages);
	memset(vmcs, 0, vmcs_descriptor.size);
	vmcs->revision_id = vmcs_descriptor.revision_id; /* vmcs revision id */
	return vmcs;
}

static struct vmcs *alloc_vmcs(void)
{
	return alloc_vmcs_cpu(smp_processor_id());
}

static void free_vmcs(struct vmcs *vmcs)
{
	free_pages((unsigned long)vmcs, vmcs_descriptor.order);
}

static __init int cpu_has_litevm_support(void)
{
	unsigned long ecx = cpuid_ecx(1);
	return test_bit(5, &ecx); /* CPUID.1:ECX.VMX[bit 5] -> VT */
}

static __exit void free_litevm_area(void)
{
	int cpu;

	for_each_online_cpu(cpu)
		free_vmcs(per_cpu(vmxarea, cpu));
}

static __init int alloc_litevm_area(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct vmcs *vmcs;

		vmcs = alloc_vmcs_cpu(cpu);
		if (!vmcs) {
			free_litevm_area();
			return -ENOMEM;
		}

		per_cpu(vmxarea, cpu) = vmcs;
	}
	return 0;
}

static __init int vmx_disabled_by_bios(void)
{
	u64 msr;

	rdmsrl(MSR_IA32_FEATURE_CONTROL, msr);
	return (msr & 5) == 1; /* locked but not enabled */
}

static __init void litevm_enable(void *garbage)
{
	int cpu = raw_smp_processor_id();
	u64 phys_addr = __pa(per_cpu(vmxarea, cpu));
	u64 old;

	rdmsrl(MSR_IA32_FEATURE_CONTROL, old);
	if ((old & 5) == 0)
		/* enable and lock */
		wrmsrl(MSR_IA32_FEATURE_CONTROL, old | 5);
	write_cr4(read_cr4() | CR4_VMXE); /* FIXME: not cpu hotplug safe */
	asm volatile ("vmxon %0" : : "m"(phys_addr) : "memory", "cc");
}

static void litevm_disable(void *garbage)
{
	asm volatile ("vmxoff" : : : "cc");
}

static int litevm_dev_open(struct inode *inode, struct file *filp)
{
	struct litevm *litevm = kzalloc(sizeof(struct litevm), GFP_KERNEL);
	int i;

	if (!litevm)
		return -ENOMEM;

	spin_lock_init(&litevm->lock);
	INIT_LIST_HEAD(&litevm->active_mmu_pages);
	for (i = 0; i < LITEVM_MAX_VCPUS; ++i) {
		struct litevm_vcpu *vcpu = &litevm->vcpus[i];

		mutex_init(&vcpu->mutex);
		vcpu->mmu.root_hpa = INVALID_PAGE;
		INIT_LIST_HEAD(&vcpu->free_pages);
	}
	filp->private_data = litevm;
	return 0;
}

/*
 * Free any memory in @free but not in @dont.
 */
static void litevm_free_physmem_slot(struct litevm_memory_slot *free,
				  struct litevm_memory_slot *dont)
{
	int i;

	if (!dont || free->phys_mem != dont->phys_mem)
		if (free->phys_mem) {
			for (i = 0; i < free->npages; ++i)
				__free_page(free->phys_mem[i]);
			vfree(free->phys_mem);
		}

	if (!dont || free->dirty_bitmap != dont->dirty_bitmap)
		vfree(free->dirty_bitmap);

	free->phys_mem = 0;
	free->npages = 0;
	free->dirty_bitmap = 0;
}

static void litevm_free_physmem(struct litevm *litevm)
{
	int i;

	for (i = 0; i < litevm->nmemslots; ++i)
		litevm_free_physmem_slot(&litevm->memslots[i], 0);
}

static void litevm_free_vmcs(struct litevm_vcpu *vcpu)
{
	if (vcpu->vmcs) {
		on_each_cpu(__vcpu_clear, vcpu, 1);
		free_vmcs(vcpu->vmcs);
		vcpu->vmcs = 0;
	}
}

static void litevm_free_vcpu(struct litevm_vcpu *vcpu)
{
	litevm_free_vmcs(vcpu);
	litevm_mmu_destroy(vcpu);
}

static void litevm_free_vcpus(struct litevm *litevm)
{
	unsigned int i;

	for (i = 0; i < LITEVM_MAX_VCPUS; ++i)
		litevm_free_vcpu(&litevm->vcpus[i]);
}

static int litevm_dev_release(struct inode *inode, struct file *filp)
{
	struct litevm *litevm = filp->private_data;

	litevm_free_vcpus(litevm);
	litevm_free_physmem(litevm);
	kfree(litevm);
	return 0;
}

unsigned long vmcs_readl(unsigned long field)
{
	unsigned long value;

	asm volatile ("vmread %1, %0" : "=g"(value) : "r"(field) : "cc");
	return value;
}

void vmcs_writel(unsigned long field, unsigned long value)
{
	u8 error;

	asm volatile ("vmwrite %1, %2; setna %0"
		       : "=g"(error) : "r"(value), "r"(field) : "cc" );
	if (error)
		printk(KERN_ERR "vmwrite error: reg %lx value %lx (err %d)\n",
		       field, value, vmcs_read32(VM_INSTRUCTION_ERROR));
}

static void vmcs_write16(unsigned long field, u16 value)
{
	vmcs_writel(field, value);
}

static void vmcs_write64(unsigned long field, u64 value)
{
#ifdef __x86_64__
	vmcs_writel(field, value);
#else
	vmcs_writel(field, value);
	asm volatile ("");
	vmcs_writel(field+1, value >> 32);
#endif
}

static void inject_gp(struct litevm_vcpu *vcpu)
{
	printk(KERN_DEBUG "inject_general_protection: rip 0x%lx\n",
	       vmcs_readl(GUEST_RIP));
	vmcs_write32(VM_ENTRY_EXCEPTION_ERROR_CODE, 0);
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD,
		     GP_VECTOR |
		     INTR_TYPE_EXCEPTION |
		     INTR_INFO_DELIEVER_CODE_MASK |
		     INTR_INFO_VALID_MASK);
}

static void update_exception_bitmap(struct litevm_vcpu *vcpu)
{
	if (vcpu->rmode.active)
		vmcs_write32(EXCEPTION_BITMAP, ~0);
	else
		vmcs_write32(EXCEPTION_BITMAP, 1 << PF_VECTOR);
}

static void enter_pmode(struct litevm_vcpu *vcpu)
{
	unsigned long flags;

	vcpu->rmode.active = 0;

	vmcs_writel(GUEST_TR_BASE, vcpu->rmode.tr.base);
	vmcs_write32(GUEST_TR_LIMIT, vcpu->rmode.tr.limit);
	vmcs_write32(GUEST_TR_AR_BYTES, vcpu->rmode.tr.ar);

	flags = vmcs_readl(GUEST_RFLAGS);
	flags &= ~(IOPL_MASK | X86_EFLAGS_VM);
	flags |= (vcpu->rmode.save_iopl << IOPL_SHIFT);
	vmcs_writel(GUEST_RFLAGS, flags);

	vmcs_writel(GUEST_CR4, (vmcs_readl(GUEST_CR4) & ~CR4_VME_MASK) |
			(vmcs_readl(CR0_READ_SHADOW) & CR4_VME_MASK) );

	update_exception_bitmap(vcpu);

	#define FIX_PMODE_DATASEG(seg, save) {				\
			vmcs_write16(GUEST_##seg##_SELECTOR, 0); 	\
			vmcs_writel(GUEST_##seg##_BASE, 0); 		\
			vmcs_write32(GUEST_##seg##_LIMIT, 0xffff);	\
			vmcs_write32(GUEST_##seg##_AR_BYTES, 0x93);	\
	}

	FIX_PMODE_DATASEG(SS, vcpu->rmode.ss);
	FIX_PMODE_DATASEG(ES, vcpu->rmode.es);
	FIX_PMODE_DATASEG(DS, vcpu->rmode.ds);
	FIX_PMODE_DATASEG(GS, vcpu->rmode.gs);
	FIX_PMODE_DATASEG(FS, vcpu->rmode.fs);

	vmcs_write16(GUEST_CS_SELECTOR,
		     vmcs_read16(GUEST_CS_SELECTOR) & ~SELECTOR_RPL_MASK);
	vmcs_write32(GUEST_CS_AR_BYTES, 0x9b);
}

static int rmode_tss_base(struct litevm* litevm)
{
	gfn_t base_gfn = litevm->memslots[0].base_gfn + litevm->memslots[0].npages - 3;
	return base_gfn << PAGE_SHIFT;
}

static void enter_rmode(struct litevm_vcpu *vcpu)
{
	unsigned long flags;

	vcpu->rmode.active = 1;

	vcpu->rmode.tr.base = vmcs_readl(GUEST_TR_BASE);
	vmcs_writel(GUEST_TR_BASE, rmode_tss_base(vcpu->litevm));

	vcpu->rmode.tr.limit = vmcs_read32(GUEST_TR_LIMIT);
	vmcs_write32(GUEST_TR_LIMIT, RMODE_TSS_SIZE - 1);

	vcpu->rmode.tr.ar = vmcs_read32(GUEST_TR_AR_BYTES);
	vmcs_write32(GUEST_TR_AR_BYTES, 0x008b);

	flags = vmcs_readl(GUEST_RFLAGS);
	vcpu->rmode.save_iopl = (flags & IOPL_MASK) >> IOPL_SHIFT;

	flags |= IOPL_MASK | X86_EFLAGS_VM;

	vmcs_writel(GUEST_RFLAGS, flags);
	vmcs_writel(GUEST_CR4, vmcs_readl(GUEST_CR4) | CR4_VME_MASK);
	update_exception_bitmap(vcpu);

	#define FIX_RMODE_SEG(seg, save) {				   \
		vmcs_write16(GUEST_##seg##_SELECTOR, 			   \
					vmcs_readl(GUEST_##seg##_BASE) >> 4); \
		vmcs_write32(GUEST_##seg##_LIMIT, 0xffff);		   \
		vmcs_write32(GUEST_##seg##_AR_BYTES, 0xf3);		   \
	}

	vmcs_write32(GUEST_CS_AR_BYTES, 0xf3);
	vmcs_write16(GUEST_CS_SELECTOR, vmcs_readl(GUEST_CS_BASE) >> 4);

	FIX_RMODE_SEG(ES, vcpu->rmode.es);
	FIX_RMODE_SEG(DS, vcpu->rmode.ds);
	FIX_RMODE_SEG(SS, vcpu->rmode.ss);
	FIX_RMODE_SEG(GS, vcpu->rmode.gs);
	FIX_RMODE_SEG(FS, vcpu->rmode.fs);
}

static int init_rmode_tss(struct litevm* litevm)
{
	struct page *p1, *p2, *p3;
	gfn_t fn = rmode_tss_base(litevm) >> PAGE_SHIFT;
	char *page;

	p1 = _gfn_to_page(litevm, fn++);
	p2 = _gfn_to_page(litevm, fn++);
	p3 = _gfn_to_page(litevm, fn);

	if (!p1 || !p2 || !p3) {
		litevm_printf(litevm,"%s: gfn_to_page failed\n", __FUNCTION__);
		return 0;
	}

	page = kmap_atomic(p1, KM_USER0);
	memset(page, 0, PAGE_SIZE);
	*(u16*)(page + 0x66) = TSS_BASE_SIZE + TSS_REDIRECTION_SIZE;
	kunmap_atomic(page, KM_USER0);

	page = kmap_atomic(p2, KM_USER0);
	memset(page, 0, PAGE_SIZE);
	kunmap_atomic(page, KM_USER0);

	page = kmap_atomic(p3, KM_USER0);
	memset(page, 0, PAGE_SIZE);
	*(page + RMODE_TSS_SIZE - 2 * PAGE_SIZE - 1) = ~0;
	kunmap_atomic(page, KM_USER0);

	return 1;
}

#ifdef __x86_64__

static void __set_efer(struct litevm_vcpu *vcpu, u64 efer)
{
	struct vmx_msr_entry *msr = find_msr_entry(vcpu, MSR_EFER);

	vcpu->shadow_efer = efer;
	if (efer & EFER_LMA) {
		vmcs_write32(VM_ENTRY_CONTROLS,
				     vmcs_read32(VM_ENTRY_CONTROLS) |
				     VM_ENTRY_CONTROLS_IA32E_MASK);
		msr->data = efer;

	} else {
		vmcs_write32(VM_ENTRY_CONTROLS,
				     vmcs_read32(VM_ENTRY_CONTROLS) &
				     ~VM_ENTRY_CONTROLS_IA32E_MASK);

		msr->data = efer & ~EFER_LME;
	}
}

static void enter_lmode(struct litevm_vcpu *vcpu)
{
	u32 guest_tr_ar;

	guest_tr_ar = vmcs_read32(GUEST_TR_AR_BYTES);
	if ((guest_tr_ar & AR_TYPE_MASK) != AR_TYPE_BUSY_64_TSS) {
		printk(KERN_DEBUG "%s: tss fixup for long mode. \n",
		       __FUNCTION__);
		vmcs_write32(GUEST_TR_AR_BYTES,
			     (guest_tr_ar & ~AR_TYPE_MASK)
			     | AR_TYPE_BUSY_64_TSS);
	}

	vcpu->shadow_efer |= EFER_LMA;

	find_msr_entry(vcpu, MSR_EFER)->data |= EFER_LMA | EFER_LME;
	vmcs_write32(VM_ENTRY_CONTROLS,
		     vmcs_read32(VM_ENTRY_CONTROLS)
		     | VM_ENTRY_CONTROLS_IA32E_MASK);
}

static void exit_lmode(struct litevm_vcpu *vcpu)
{
	vcpu->shadow_efer &= ~EFER_LMA;

	vmcs_write32(VM_ENTRY_CONTROLS,
		     vmcs_read32(VM_ENTRY_CONTROLS)
		     & ~VM_ENTRY_CONTROLS_IA32E_MASK);
}

#endif

static void __set_cr0(struct litevm_vcpu *vcpu, unsigned long cr0)
{
	if (vcpu->rmode.active && (cr0 & CR0_PE_MASK))
		enter_pmode(vcpu);

	if (!vcpu->rmode.active && !(cr0 & CR0_PE_MASK))
		enter_rmode(vcpu);

#ifdef __x86_64__
	if (vcpu->shadow_efer & EFER_LME) {
		if (!is_paging() && (cr0 & CR0_PG_MASK))
			enter_lmode(vcpu);
		if (is_paging() && !(cr0 & CR0_PG_MASK))
			exit_lmode(vcpu);
	}
#endif

	vmcs_writel(CR0_READ_SHADOW, cr0);
	vmcs_writel(GUEST_CR0, cr0 | LITEVM_VM_CR0_ALWAYS_ON);
}

static int pdptrs_have_reserved_bits_set(struct litevm_vcpu *vcpu,
					 unsigned long cr3)
{
	gfn_t pdpt_gfn = cr3 >> PAGE_SHIFT;
	unsigned offset = (cr3 & (PAGE_SIZE-1)) >> 5;
	int i;
	u64 pdpte;
	u64 *pdpt;
	struct litevm_memory_slot *memslot;

	spin_lock(&vcpu->litevm->lock);
	memslot = gfn_to_memslot(vcpu->litevm, pdpt_gfn);
	/* FIXME: !memslot - emulate? 0xff? */
	pdpt = kmap_atomic(gfn_to_page(memslot, pdpt_gfn), KM_USER0);

	for (i = 0; i < 4; ++i) {
		pdpte = pdpt[offset + i];
		if ((pdpte & 1) && (pdpte & 0xfffffff0000001e6ull))
			break;
	}

	kunmap_atomic(pdpt, KM_USER0);
	spin_unlock(&vcpu->litevm->lock);

	return i != 4;
}

static void set_cr0(struct litevm_vcpu *vcpu, unsigned long cr0)
{
	if (cr0 & CR0_RESEVED_BITS) {
		printk(KERN_DEBUG "set_cr0: 0x%lx #GP, reserved bits 0x%lx\n",
		       cr0, guest_cr0());
		inject_gp(vcpu);
		return;
	}

	if ((cr0 & CR0_NW_MASK) && !(cr0 & CR0_CD_MASK)) {
		printk(KERN_DEBUG "set_cr0: #GP, CD == 0 && NW == 1\n");
		inject_gp(vcpu);
		return;
	}

	if ((cr0 & CR0_PG_MASK) && !(cr0 & CR0_PE_MASK)) {
		printk(KERN_DEBUG "set_cr0: #GP, set PG flag "
		       "and a clear PE flag\n");
		inject_gp(vcpu);
		return;
	}

	if (!is_paging() && (cr0 & CR0_PG_MASK)) {
#ifdef __x86_64__
		if ((vcpu->shadow_efer & EFER_LME)) {
			u32 guest_cs_ar;
			if (!is_pae()) {
				printk(KERN_DEBUG "set_cr0: #GP, start paging "
				       "in long mode while PAE is disabled\n");
				inject_gp(vcpu);
				return;
			}
			guest_cs_ar = vmcs_read32(GUEST_CS_AR_BYTES);
			if (guest_cs_ar & SEGMENT_AR_L_MASK) {
				printk(KERN_DEBUG "set_cr0: #GP, start paging "
				       "in long mode while CS.L == 1\n");
				inject_gp(vcpu);
				return;

			}
		} else
#endif
		if (is_pae() &&
			    pdptrs_have_reserved_bits_set(vcpu, vcpu->cr3)) {
			printk(KERN_DEBUG "set_cr0: #GP, pdptrs "
			       "reserved bits\n");
			inject_gp(vcpu);
			return;
		}

	}

	__set_cr0(vcpu, cr0);
	litevm_mmu_reset_context(vcpu);
	return;
}

static void lmsw(struct litevm_vcpu *vcpu, unsigned long msw)
{
	unsigned long cr0 = guest_cr0();

	if ((msw & CR0_PE_MASK) && !(cr0 & CR0_PE_MASK)) {
		enter_pmode(vcpu);
		vmcs_writel(CR0_READ_SHADOW, cr0 | CR0_PE_MASK);

	} else
		printk(KERN_DEBUG "lmsw: unexpected\n");

	vmcs_writel(GUEST_CR0, (vmcs_readl(GUEST_CR0) & ~LMSW_GUEST_MASK)
				| (msw & LMSW_GUEST_MASK));
}

static void __set_cr4(struct litevm_vcpu *vcpu, unsigned long cr4)
{
	vmcs_writel(CR4_READ_SHADOW, cr4);
	vmcs_writel(GUEST_CR4, cr4 | (vcpu->rmode.active ?
		    LITEVM_RMODE_VM_CR4_ALWAYS_ON : LITEVM_PMODE_VM_CR4_ALWAYS_ON));
}

static void set_cr4(struct litevm_vcpu *vcpu, unsigned long cr4)
{
	if (cr4 & CR4_RESEVED_BITS) {
		printk(KERN_DEBUG "set_cr4: #GP, reserved bits\n");
		inject_gp(vcpu);
		return;
	}

	if (is_long_mode()) {
		if (!(cr4 & CR4_PAE_MASK)) {
			printk(KERN_DEBUG "set_cr4: #GP, clearing PAE while "
			       "in long mode\n");
			inject_gp(vcpu);
			return;
		}
	} else if (is_paging() && !is_pae() && (cr4 & CR4_PAE_MASK)
		   && pdptrs_have_reserved_bits_set(vcpu, vcpu->cr3)) {
		printk(KERN_DEBUG "set_cr4: #GP, pdptrs reserved bits\n");
		inject_gp(vcpu);
	}

	if (cr4 & CR4_VMXE_MASK) {
		printk(KERN_DEBUG "set_cr4: #GP, setting VMXE\n");
		inject_gp(vcpu);
		return;
	}
	__set_cr4(vcpu, cr4);
	spin_lock(&vcpu->litevm->lock);
	litevm_mmu_reset_context(vcpu);
	spin_unlock(&vcpu->litevm->lock);
}

static void set_cr3(struct litevm_vcpu *vcpu, unsigned long cr3)
{
	if (is_long_mode()) {
		if ( cr3 & CR3_L_MODE_RESEVED_BITS) {
			printk(KERN_DEBUG "set_cr3: #GP, reserved bits\n");
			inject_gp(vcpu);
			return;
		}
	} else {
		if (cr3 & CR3_RESEVED_BITS) {
			printk(KERN_DEBUG "set_cr3: #GP, reserved bits\n");
			inject_gp(vcpu);
			return;
		}
		if (is_paging() && is_pae() &&
		    pdptrs_have_reserved_bits_set(vcpu, cr3)) {
			printk(KERN_DEBUG "set_cr3: #GP, pdptrs "
			       "reserved bits\n");
			inject_gp(vcpu);
			return;
		}
	}

	vcpu->cr3 = cr3;
	spin_lock(&vcpu->litevm->lock);
	vcpu->mmu.new_cr3(vcpu);
	spin_unlock(&vcpu->litevm->lock);
}

static void set_cr8(struct litevm_vcpu *vcpu, unsigned long cr8)
{
	if ( cr8 & CR8_RESEVED_BITS) {
		printk(KERN_DEBUG "set_cr8: #GP, reserved bits 0x%lx\n", cr8);
		inject_gp(vcpu);
		return;
	}
	vcpu->cr8 = cr8;
}

static u32 get_rdx_init_val(void)
{
	u32 val;

	asm ("movl $1, %%eax \n\t"
	     "movl %%eax, %0 \n\t" : "=g"(val) );
	return val;

}

static void fx_init(struct litevm_vcpu *vcpu)
{
	struct __attribute__ ((__packed__)) fx_image_s {
		u16 control; //fcw
		u16 status; //fsw
		u16 tag; // ftw
		u16 opcode; //fop
		u64 ip; // fpu ip
		u64 operand;// fpu dp
		u32 mxcsr;
		u32 mxcsr_mask;

	} *fx_image;

	fx_save(vcpu->host_fx_image);
	fpu_init();
	fx_save(vcpu->guest_fx_image);
	fx_restore(vcpu->host_fx_image);

	fx_image = (struct fx_image_s *)vcpu->guest_fx_image;
	fx_image->mxcsr = 0x1f80;
	memset(vcpu->guest_fx_image + sizeof(struct fx_image_s),
	       0, FX_IMAGE_SIZE - sizeof(struct fx_image_s));
}

static void vmcs_write32_fixedbits(u32 msr, u32 vmcs_field, u32 val)
{
	u32 msr_high, msr_low;

	rdmsr(msr, msr_low, msr_high);

	val &= msr_high;
	val |= msr_low;
	vmcs_write32(vmcs_field, val);
}

/*
 * Sets up the vmcs for emulated real mode.
 */
static int litevm_vcpu_setup(struct litevm_vcpu *vcpu)
{
	extern asmlinkage void litevm_vmx_return(void);
	u32 host_sysenter_cs;
	u32 junk;
	unsigned long a;
	struct descriptor_table dt;
	int i;
	int ret;
	u64 tsc;
	int nr_good_msrs;


	if (!init_rmode_tss(vcpu->litevm)) {
		ret = 0;
		goto out;
	}

	memset(vcpu->regs, 0, sizeof(vcpu->regs));
	vcpu->regs[VCPU_REGS_RDX] = get_rdx_init_val();
	vcpu->cr8 = 0;
	vcpu->apic_base = 0xfee00000 |
			/*for vcpu 0*/ MSR_IA32_APICBASE_BSP |
			MSR_IA32_APICBASE_ENABLE;

	fx_init(vcpu);

#define SEG_SETUP(seg) do {					\
		vmcs_write16(GUEST_##seg##_SELECTOR, 0);	\
		vmcs_writel(GUEST_##seg##_BASE, 0);		\
		vmcs_write32(GUEST_##seg##_LIMIT, 0xffff);	\
		vmcs_write32(GUEST_##seg##_AR_BYTES, 0x93); 	\
	} while (0)

	/*
	 * GUEST_CS_BASE should really be 0xffff0000, but VT vm86 mode
	 * insists on having GUEST_CS_BASE == GUEST_CS_SELECTOR << 4.  Sigh.
	 */
	vmcs_write16(GUEST_CS_SELECTOR, 0xf000);
	vmcs_writel(GUEST_CS_BASE, 0x000f0000);
	vmcs_write32(GUEST_CS_LIMIT, 0xffff);
	vmcs_write32(GUEST_CS_AR_BYTES, 0x9b);

	SEG_SETUP(DS);
	SEG_SETUP(ES);
	SEG_SETUP(FS);
	SEG_SETUP(GS);
	SEG_SETUP(SS);

	vmcs_write16(GUEST_TR_SELECTOR, 0);
	vmcs_writel(GUEST_TR_BASE, 0);
	vmcs_write32(GUEST_TR_LIMIT, 0xffff);
	vmcs_write32(GUEST_TR_AR_BYTES, 0x008b);

	vmcs_write16(GUEST_LDTR_SELECTOR, 0);
	vmcs_writel(GUEST_LDTR_BASE, 0);
	vmcs_write32(GUEST_LDTR_LIMIT, 0xffff);
	vmcs_write32(GUEST_LDTR_AR_BYTES, 0x00082);

	vmcs_write32(GUEST_SYSENTER_CS, 0);
	vmcs_writel(GUEST_SYSENTER_ESP, 0);
	vmcs_writel(GUEST_SYSENTER_EIP, 0);

	vmcs_writel(GUEST_RFLAGS, 0x02);
	vmcs_writel(GUEST_RIP, 0xfff0);
	vmcs_writel(GUEST_RSP, 0);

	vmcs_writel(GUEST_CR3, 0);

	//todo: dr0 = dr1 = dr2 = dr3 = 0; dr6 = 0xffff0ff0
	vmcs_writel(GUEST_DR7, 0x400);

	vmcs_writel(GUEST_GDTR_BASE, 0);
	vmcs_write32(GUEST_GDTR_LIMIT, 0xffff);

	vmcs_writel(GUEST_IDTR_BASE, 0);
	vmcs_write32(GUEST_IDTR_LIMIT, 0xffff);

	vmcs_write32(GUEST_ACTIVITY_STATE, 0);
	vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, 0);
	vmcs_write32(GUEST_PENDING_DBG_EXCEPTIONS, 0);

	/* I/O */
	vmcs_write64(IO_BITMAP_A, 0);
	vmcs_write64(IO_BITMAP_B, 0);

	rdtscll(tsc);
	vmcs_write64(TSC_OFFSET, -tsc);

	vmcs_write64(VMCS_LINK_POINTER, -1ull); /* 22.3.1.5 */

	/* Special registers */
	vmcs_write64(GUEST_IA32_DEBUGCTL, 0);

	/* Control */
	vmcs_write32_fixedbits(MSR_IA32_VMX_PINBASED_CTLS_MSR,
			       PIN_BASED_VM_EXEC_CONTROL,
			       PIN_BASED_EXT_INTR_MASK   /* 20.6.1 */
			       | PIN_BASED_NMI_EXITING   /* 20.6.1 */
			);
	vmcs_write32_fixedbits(MSR_IA32_VMX_PROCBASED_CTLS_MSR,
			       CPU_BASED_VM_EXEC_CONTROL,
			       CPU_BASED_HLT_EXITING         /* 20.6.2 */
			       | CPU_BASED_CR8_LOAD_EXITING    /* 20.6.2 */
			       | CPU_BASED_CR8_STORE_EXITING   /* 20.6.2 */
			       | CPU_BASED_UNCOND_IO_EXITING   /* 20.6.2 */
			       | CPU_BASED_INVDPG_EXITING
			       | CPU_BASED_MOV_DR_EXITING
			       | CPU_BASED_USE_TSC_OFFSETING   /* 21.3 */
			);

	vmcs_write32(EXCEPTION_BITMAP, 1 << PF_VECTOR);
	vmcs_write32(PAGE_FAULT_ERROR_CODE_MASK, 0);
	vmcs_write32(PAGE_FAULT_ERROR_CODE_MATCH, 0);
	vmcs_write32(CR3_TARGET_COUNT, 0);           /* 22.2.1 */

	vmcs_writel(HOST_CR0, read_cr0());  /* 22.2.3 */
	vmcs_writel(HOST_CR4, read_cr4());  /* 22.2.3, 22.2.5 */
	vmcs_writel(HOST_CR3, read_cr3());  /* 22.2.3  FIXME: shadow tables */

	vmcs_write16(HOST_CS_SELECTOR, __KERNEL_CS);  /* 22.2.4 */
	vmcs_write16(HOST_DS_SELECTOR, __KERNEL_DS);  /* 22.2.4 */
	vmcs_write16(HOST_ES_SELECTOR, __KERNEL_DS);  /* 22.2.4 */
	vmcs_write16(HOST_FS_SELECTOR, read_fs());    /* 22.2.4 */
	vmcs_write16(HOST_GS_SELECTOR, read_gs());    /* 22.2.4 */
	vmcs_write16(HOST_SS_SELECTOR, __KERNEL_DS);  /* 22.2.4 */
#ifdef __x86_64__
	rdmsrl(MSR_FS_BASE, a);
	vmcs_writel(HOST_FS_BASE, a); /* 22.2.4 */
	rdmsrl(MSR_GS_BASE, a);
	vmcs_writel(HOST_GS_BASE, a); /* 22.2.4 */
#else
	vmcs_writel(HOST_FS_BASE, 0); /* 22.2.4 */
	vmcs_writel(HOST_GS_BASE, 0); /* 22.2.4 */
#endif

	vmcs_write16(HOST_TR_SELECTOR, GDT_ENTRY_TSS*8);  /* 22.2.4 */

	get_idt(&dt);
	vmcs_writel(HOST_IDTR_BASE, dt.base);   /* 22.2.4 */


	vmcs_writel(HOST_RIP, (unsigned long)litevm_vmx_return); /* 22.2.5 */

	rdmsr(MSR_IA32_SYSENTER_CS, host_sysenter_cs, junk);
	vmcs_write32(HOST_IA32_SYSENTER_CS, host_sysenter_cs);
	rdmsrl(MSR_IA32_SYSENTER_ESP, a);
	vmcs_writel(HOST_IA32_SYSENTER_ESP, a);   /* 22.2.3 */
	rdmsrl(MSR_IA32_SYSENTER_EIP, a);
	vmcs_writel(HOST_IA32_SYSENTER_EIP, a);   /* 22.2.3 */

	ret = -ENOMEM;
	vcpu->guest_msrs = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!vcpu->guest_msrs)
		goto out;
	vcpu->host_msrs = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!vcpu->host_msrs)
		goto out_free_guest_msrs;

	for (i = 0; i < NR_VMX_MSR; ++i) {
		u32 index = vmx_msr_index[i];
		u32 data_low, data_high;
		u64 data;
		int j = vcpu->nmsrs;

		if (rdmsr_safe(index, &data_low, &data_high) < 0)
			continue;
		data = data_low | ((u64)data_high << 32);
		vcpu->host_msrs[j].index = index;
		vcpu->host_msrs[j].reserved = 0;
		vcpu->host_msrs[j].data = data;
		vcpu->guest_msrs[j] = vcpu->host_msrs[j];
		++vcpu->nmsrs;
	}
	printk("msrs: %d\n", vcpu->nmsrs);

	nr_good_msrs = vcpu->nmsrs - NR_BAD_MSRS;
	vmcs_writel(VM_ENTRY_MSR_LOAD_ADDR,
		    virt_to_phys(vcpu->guest_msrs + NR_BAD_MSRS));
	vmcs_writel(VM_EXIT_MSR_STORE_ADDR,
		    virt_to_phys(vcpu->guest_msrs + NR_BAD_MSRS));
	vmcs_writel(VM_EXIT_MSR_LOAD_ADDR,
		    virt_to_phys(vcpu->host_msrs + NR_BAD_MSRS));
	vmcs_write32_fixedbits(MSR_IA32_VMX_EXIT_CTLS_MSR, VM_EXIT_CONTROLS,
		     	       (HOST_IS_64 << 9));  /* 22.2,1, 20.7.1 */
	vmcs_write32(VM_EXIT_MSR_STORE_COUNT, nr_good_msrs); /* 22.2.2 */
	vmcs_write32(VM_EXIT_MSR_LOAD_COUNT, nr_good_msrs);  /* 22.2.2 */
	vmcs_write32(VM_ENTRY_MSR_LOAD_COUNT, nr_good_msrs); /* 22.2.2 */


	/* 22.2.1, 20.8.1 */
	vmcs_write32_fixedbits(MSR_IA32_VMX_ENTRY_CTLS_MSR,
                               VM_ENTRY_CONTROLS, 0);
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);  /* 22.2.1 */

	vmcs_writel(VIRTUAL_APIC_PAGE_ADDR, 0);
	vmcs_writel(TPR_THRESHOLD, 0);

	vmcs_writel(CR0_GUEST_HOST_MASK, LITEVM_GUEST_CR0_MASK);
	vmcs_writel(CR4_GUEST_HOST_MASK, LITEVM_GUEST_CR4_MASK);

	__set_cr0(vcpu, 0x60000010); // enter rmode
	__set_cr4(vcpu, 0);
#ifdef __x86_64__
	__set_efer(vcpu, 0);
#endif

	ret = litevm_mmu_init(vcpu);

	return ret;

out_free_guest_msrs:
	kfree(vcpu->guest_msrs);
out:
	return ret;
}

/*
 * Sync the rsp and rip registers into the vcpu structure.  This allows
 * registers to be accessed by indexing vcpu->regs.
 */
static void vcpu_load_rsp_rip(struct litevm_vcpu *vcpu)
{
	vcpu->regs[VCPU_REGS_RSP] = vmcs_readl(GUEST_RSP);
	vcpu->rip = vmcs_readl(GUEST_RIP);
}

/*
 * Syncs rsp and rip back into the vmcs.  Should be called after possible
 * modification.
 */
static void vcpu_put_rsp_rip(struct litevm_vcpu *vcpu)
{
	vmcs_writel(GUEST_RSP, vcpu->regs[VCPU_REGS_RSP]);
	vmcs_writel(GUEST_RIP, vcpu->rip);
}

/*
 * Creates some virtual cpus.  Good luck creating more than one.
 */
static int litevm_dev_ioctl_create_vcpu(struct litevm *litevm, int n)
{
	int r;
	struct litevm_vcpu *vcpu;
	struct vmcs *vmcs;

	r = -EINVAL;
	if (n < 0 || n >= LITEVM_MAX_VCPUS)
		goto out;

	vcpu = &litevm->vcpus[n];

	mutex_lock(&vcpu->mutex);

	if (vcpu->vmcs) {
		mutex_unlock(&vcpu->mutex);
		return -EEXIST;
	}

	vcpu->host_fx_image = (char*)ALIGN((hva_t)vcpu->fx_buf,
					   FX_IMAGE_ALIGN);
	vcpu->guest_fx_image = vcpu->host_fx_image + FX_IMAGE_SIZE;

	vcpu->cpu = -1;  /* First load will set up TR */
	vcpu->litevm = litevm;
	vmcs = alloc_vmcs();
	if (!vmcs) {
		mutex_unlock(&vcpu->mutex);
		goto out_free_vcpus;
	}
	vmcs_clear(vmcs);
	vcpu->vmcs = vmcs;
	vcpu->launched = 0;

	__vcpu_load(vcpu);

	r = litevm_vcpu_setup(vcpu);

	vcpu_put(vcpu);

	if (r < 0)
		goto out_free_vcpus;

	return 0;

out_free_vcpus:
	litevm_free_vcpu(vcpu);
out:
	return r;
}

/*
 * Allocate some memory and give it an address in the guest physical address
 * space.
 *
 * Discontiguous memory is allowed, mostly for framebuffers.
 */
static int litevm_dev_ioctl_set_memory_region(struct litevm *litevm,
					   struct litevm_memory_region *mem)
{
	int r;
	gfn_t base_gfn;
	unsigned long npages;
	unsigned long i;
	struct litevm_memory_slot *memslot;
	struct litevm_memory_slot old, new;
	int memory_config_version;

	r = -EINVAL;
	/* General sanity checks */
	if (mem->memory_size & (PAGE_SIZE - 1))
		goto out;
	if (mem->guest_phys_addr & (PAGE_SIZE - 1))
		goto out;
	if (mem->slot >= LITEVM_MEMORY_SLOTS)
		goto out;
	if (mem->guest_phys_addr + mem->memory_size < mem->guest_phys_addr)
		goto out;

	memslot = &litevm->memslots[mem->slot];
	base_gfn = mem->guest_phys_addr >> PAGE_SHIFT;
	npages = mem->memory_size >> PAGE_SHIFT;

	if (!npages)
		mem->flags &= ~LITEVM_MEM_LOG_DIRTY_PAGES;

raced:
	spin_lock(&litevm->lock);

	memory_config_version = litevm->memory_config_version;
	new = old = *memslot;

	new.base_gfn = base_gfn;
	new.npages = npages;
	new.flags = mem->flags;

	/* Disallow changing a memory slot's size. */
	r = -EINVAL;
	if (npages && old.npages && npages != old.npages)
		goto out_unlock;

	/* Check for overlaps */
	r = -EEXIST;
	for (i = 0; i < LITEVM_MEMORY_SLOTS; ++i) {
		struct litevm_memory_slot *s = &litevm->memslots[i];

		if (s == memslot)
			continue;
		if (!((base_gfn + npages <= s->base_gfn) ||
		      (base_gfn >= s->base_gfn + s->npages)))
			goto out_unlock;
	}
	/*
	 * Do memory allocations outside lock.  memory_config_version will
	 * detect any races.
	 */
	spin_unlock(&litevm->lock);

	/* Deallocate if slot is being removed */
	if (!npages)
		new.phys_mem = 0;

	/* Free page dirty bitmap if unneeded */
	if (!(new.flags & LITEVM_MEM_LOG_DIRTY_PAGES))
		new.dirty_bitmap = 0;

	r = -ENOMEM;

	/* Allocate if a slot is being created */
	if (npages && !new.phys_mem) {
		new.phys_mem = vmalloc(npages * sizeof(struct page *));

		if (!new.phys_mem)
			goto out_free;

		memset(new.phys_mem, 0, npages * sizeof(struct page *));
		for (i = 0; i < npages; ++i) {
			new.phys_mem[i] = alloc_page(GFP_HIGHUSER);
			if (!new.phys_mem[i])
				goto out_free;
		}
	}

	/* Allocate page dirty bitmap if needed */
	if ((new.flags & LITEVM_MEM_LOG_DIRTY_PAGES) && !new.dirty_bitmap) {
		unsigned dirty_bytes = ALIGN(npages, BITS_PER_LONG) / 8;

		new.dirty_bitmap = vmalloc(dirty_bytes);
		if (!new.dirty_bitmap)
			goto out_free;
		memset(new.dirty_bitmap, 0, dirty_bytes);
	}

	spin_lock(&litevm->lock);

	if (memory_config_version != litevm->memory_config_version) {
		spin_unlock(&litevm->lock);
		litevm_free_physmem_slot(&new, &old);
		goto raced;
	}

	r = -EAGAIN;
	if (litevm->busy)
		goto out_unlock;

	if (mem->slot >= litevm->nmemslots)
		litevm->nmemslots = mem->slot + 1;

	*memslot = new;
	++litevm->memory_config_version;

	spin_unlock(&litevm->lock);

	for (i = 0; i < LITEVM_MAX_VCPUS; ++i) {
		struct litevm_vcpu *vcpu;

		vcpu = vcpu_load(litevm, i);
		if (!vcpu)
			continue;
		litevm_mmu_reset_context(vcpu);
		vcpu_put(vcpu);
	}

	litevm_free_physmem_slot(&old, &new);
	return 0;

out_unlock:
	spin_unlock(&litevm->lock);
out_free:
	litevm_free_physmem_slot(&new, &old);
out:
	return r;
}

/*
 * Get (and clear) the dirty memory log for a memory slot.
 */
static int litevm_dev_ioctl_get_dirty_log(struct litevm *litevm,
				       struct litevm_dirty_log *log)
{
	struct litevm_memory_slot *memslot;
	int r, i;
	int n;
	unsigned long any = 0;

	spin_lock(&litevm->lock);

	/*
	 * Prevent changes to guest memory configuration even while the lock
	 * is not taken.
	 */
	++litevm->busy;
	spin_unlock(&litevm->lock);
	r = -EINVAL;
	if (log->slot >= LITEVM_MEMORY_SLOTS)
		goto out;

	memslot = &litevm->memslots[log->slot];
	r = -ENOENT;
	if (!memslot->dirty_bitmap)
		goto out;

	n = ALIGN(memslot->npages, 8) / 8;

	for (i = 0; !any && i < n; ++i)
		any = memslot->dirty_bitmap[i];

	r = -EFAULT;
	if (copy_to_user(log->dirty_bitmap, memslot->dirty_bitmap, n))
		goto out;


	if (any) {
		spin_lock(&litevm->lock);
		litevm_mmu_slot_remove_write_access(litevm, log->slot);
		spin_unlock(&litevm->lock);
		memset(memslot->dirty_bitmap, 0, n);
		for (i = 0; i < LITEVM_MAX_VCPUS; ++i) {
			struct litevm_vcpu *vcpu = vcpu_load(litevm, i);

			if (!vcpu)
				continue;
			flush_guest_tlb(vcpu);
			vcpu_put(vcpu);
		}
	}

	r = 0;

out:
	spin_lock(&litevm->lock);
	--litevm->busy;
	spin_unlock(&litevm->lock);
	return r;
}

struct litevm_memory_slot *gfn_to_memslot(struct litevm *litevm, gfn_t gfn)
{
	int i;

	for (i = 0; i < litevm->nmemslots; ++i) {
		struct litevm_memory_slot *memslot = &litevm->memslots[i];

		if (gfn >= memslot->base_gfn
		    && gfn < memslot->base_gfn + memslot->npages)
			return memslot;
	}
	return 0;
}

void mark_page_dirty(struct litevm *litevm, gfn_t gfn)
{
	int i;
	struct litevm_memory_slot *memslot = 0;
	unsigned long rel_gfn;

	for (i = 0; i < litevm->nmemslots; ++i) {
		memslot = &litevm->memslots[i];

		if (gfn >= memslot->base_gfn
		    && gfn < memslot->base_gfn + memslot->npages) {

			if (!memslot || !memslot->dirty_bitmap)
				return;

			rel_gfn = gfn - memslot->base_gfn;

			/* avoid RMW */
			if (!test_bit(rel_gfn, memslot->dirty_bitmap))
				set_bit(rel_gfn, memslot->dirty_bitmap);
			return;
		}
	}
}

static void skip_emulated_instruction(struct litevm_vcpu *vcpu)
{
	unsigned long rip;
	u32 interruptibility;

	rip = vmcs_readl(GUEST_RIP);
	rip += vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	vmcs_writel(GUEST_RIP, rip);

	/*
	 * We emulated an instruction, so temporary interrupt blocking
	 * should be removed, if set.
	 */
	interruptibility = vmcs_read32(GUEST_INTERRUPTIBILITY_INFO);
	if (interruptibility & 3)
		vmcs_write32(GUEST_INTERRUPTIBILITY_INFO,
			     interruptibility & ~3);
}

static int emulator_read_std(unsigned long addr,
			     unsigned long *val,
			     unsigned int bytes,
			     struct x86_emulate_ctxt *ctxt)
{
	struct litevm_vcpu *vcpu = ctxt->vcpu;
	void *data = val;

	while (bytes) {
		gpa_t gpa = vcpu->mmu.gva_to_gpa(vcpu, addr);
		unsigned offset = addr & (PAGE_SIZE-1);
		unsigned tocopy = min(bytes, (unsigned)PAGE_SIZE - offset);
		unsigned long pfn;
		struct litevm_memory_slot *memslot;
		void *page;

		if (gpa == UNMAPPED_GVA)
			return X86EMUL_PROPAGATE_FAULT;
		pfn = gpa >> PAGE_SHIFT;
		memslot = gfn_to_memslot(vcpu->litevm, pfn);
		if (!memslot)
			return X86EMUL_UNHANDLEABLE;
		page = kmap_atomic(gfn_to_page(memslot, pfn), KM_USER0);

		memcpy(data, page + offset, tocopy);

		kunmap_atomic(page, KM_USER0);

		bytes -= tocopy;
		data += tocopy;
		addr += tocopy;
	}

	return X86EMUL_CONTINUE;
}

static int emulator_write_std(unsigned long addr,
			      unsigned long val,
			      unsigned int bytes,
			      struct x86_emulate_ctxt *ctxt)
{
	printk(KERN_ERR "emulator_write_std: addr %lx n %d\n",
	       addr, bytes);
	return X86EMUL_UNHANDLEABLE;
}

static int emulator_read_emulated(unsigned long addr,
				  unsigned long *val,
				  unsigned int bytes,
				  struct x86_emulate_ctxt *ctxt)
{
	struct litevm_vcpu *vcpu = ctxt->vcpu;

	if (vcpu->mmio_read_completed) {
		memcpy(val, vcpu->mmio_data, bytes);
		vcpu->mmio_read_completed = 0;
		return X86EMUL_CONTINUE;
	} else if (emulator_read_std(addr, val, bytes, ctxt)
		   == X86EMUL_CONTINUE)
		return X86EMUL_CONTINUE;
	else {
		gpa_t gpa = vcpu->mmu.gva_to_gpa(vcpu, addr);
		if (gpa == UNMAPPED_GVA)
			return vcpu_printf(vcpu, "not present\n"), X86EMUL_PROPAGATE_FAULT;
		vcpu->mmio_needed = 1;
		vcpu->mmio_phys_addr = gpa;
		vcpu->mmio_size = bytes;
		vcpu->mmio_is_write = 0;

		return X86EMUL_UNHANDLEABLE;
	}
}

static int emulator_write_emulated(unsigned long addr,
				   unsigned long val,
				   unsigned int bytes,
				   struct x86_emulate_ctxt *ctxt)
{
	struct litevm_vcpu *vcpu = ctxt->vcpu;
	gpa_t gpa = vcpu->mmu.gva_to_gpa(vcpu, addr);

	if (gpa == UNMAPPED_GVA)
		return X86EMUL_PROPAGATE_FAULT;

	vcpu->mmio_needed = 1;
	vcpu->mmio_phys_addr = gpa;
	vcpu->mmio_size = bytes;
	vcpu->mmio_is_write = 1;
	memcpy(vcpu->mmio_data, &val, bytes);

	return X86EMUL_CONTINUE;
}

static int emulator_cmpxchg_emulated(unsigned long addr,
				     unsigned long old,
				     unsigned long new,
				     unsigned int bytes,
				     struct x86_emulate_ctxt *ctxt)
{
	static int reported;

	if (!reported) {
		reported = 1;
		printk(KERN_WARNING "litevm: emulating exchange as write\n");
	}
	return emulator_write_emulated(addr, new, bytes, ctxt);
}

static void report_emulation_failure(struct x86_emulate_ctxt *ctxt)
{
	static int reported;
	u8 opcodes[4];
	unsigned long rip = vmcs_readl(GUEST_RIP);
	unsigned long rip_linear = rip + vmcs_readl(GUEST_CS_BASE);

	if (reported)
		return;

	emulator_read_std(rip_linear, (void *)opcodes, 4, ctxt);

	printk(KERN_ERR "emulation failed but !mmio_needed?"
	       " rip %lx %02x %02x %02x %02x\n",
	       rip, opcodes[0], opcodes[1], opcodes[2], opcodes[3]);
	reported = 1;
}

struct x86_emulate_ops emulate_ops = {
	.read_std            = emulator_read_std,
	.write_std           = emulator_write_std,
	.read_emulated       = emulator_read_emulated,
	.write_emulated      = emulator_write_emulated,
	.cmpxchg_emulated    = emulator_cmpxchg_emulated,
};

enum emulation_result {
	EMULATE_DONE,       /* no further processing */
	EMULATE_DO_MMIO,      /* litevm_run filled with mmio request */
	EMULATE_FAIL,         /* can't emulate this instruction */
};

static int emulate_instruction(struct litevm_vcpu *vcpu,
			       struct litevm_run *run,
			       unsigned long cr2,
			       u16 error_code)
{
	struct x86_emulate_ctxt emulate_ctxt;
	int r;
	u32 cs_ar;

	vcpu_load_rsp_rip(vcpu);

	cs_ar = vmcs_read32(GUEST_CS_AR_BYTES);

	emulate_ctxt.vcpu = vcpu;
	emulate_ctxt.eflags = vmcs_readl(GUEST_RFLAGS);
	emulate_ctxt.cr2 = cr2;
	emulate_ctxt.mode = (emulate_ctxt.eflags & X86_EFLAGS_VM)
		? X86EMUL_MODE_REAL : (cs_ar & AR_L_MASK)
		? X86EMUL_MODE_PROT64 :	(cs_ar & AR_DB_MASK)
		? X86EMUL_MODE_PROT32 : X86EMUL_MODE_PROT16;

	if (emulate_ctxt.mode == X86EMUL_MODE_PROT64) {
		emulate_ctxt.cs_base = 0;
		emulate_ctxt.ds_base = 0;
		emulate_ctxt.es_base = 0;
		emulate_ctxt.ss_base = 0;
		emulate_ctxt.gs_base = 0;
		emulate_ctxt.fs_base = 0;
	} else {
		emulate_ctxt.cs_base = vmcs_readl(GUEST_CS_BASE);
		emulate_ctxt.ds_base = vmcs_readl(GUEST_DS_BASE);
		emulate_ctxt.es_base = vmcs_readl(GUEST_ES_BASE);
		emulate_ctxt.ss_base = vmcs_readl(GUEST_SS_BASE);
		emulate_ctxt.gs_base = vmcs_readl(GUEST_GS_BASE);
		emulate_ctxt.fs_base = vmcs_readl(GUEST_FS_BASE);
	}

	vcpu->mmio_is_write = 0;
	r = x86_emulate_memop(&emulate_ctxt, &emulate_ops);

	if ((r || vcpu->mmio_is_write) && run) {
		run->mmio.phys_addr = vcpu->mmio_phys_addr;
		memcpy(run->mmio.data, vcpu->mmio_data, 8);
		run->mmio.len = vcpu->mmio_size;
		run->mmio.is_write = vcpu->mmio_is_write;
	}

	if (r) {
		if (!vcpu->mmio_needed) {
			report_emulation_failure(&emulate_ctxt);
			return EMULATE_FAIL;
		}
		return EMULATE_DO_MMIO;
	}

	vcpu_put_rsp_rip(vcpu);
	vmcs_writel(GUEST_RFLAGS, emulate_ctxt.eflags);

	if (vcpu->mmio_is_write)
		return EMULATE_DO_MMIO;

	return EMULATE_DONE;
}

static u64 mk_cr_64(u64 curr_cr, u32 new_val)
{
	return (curr_cr & ~((1ULL << 32) - 1)) | new_val;
}

void realmode_lgdt(struct litevm_vcpu *vcpu, u16 limit, unsigned long base)
{
	vmcs_writel(GUEST_GDTR_BASE, base);
	vmcs_write32(GUEST_GDTR_LIMIT, limit);
}

void realmode_lidt(struct litevm_vcpu *vcpu, u16 limit, unsigned long base)
{
	vmcs_writel(GUEST_IDTR_BASE, base);
	vmcs_write32(GUEST_IDTR_LIMIT, limit);
}

void realmode_lmsw(struct litevm_vcpu *vcpu, unsigned long msw,
		   unsigned long *rflags)
{
	lmsw(vcpu, msw);
	*rflags = vmcs_readl(GUEST_RFLAGS);
}

unsigned long realmode_get_cr(struct litevm_vcpu *vcpu, int cr)
{
	switch (cr) {
	case 0:
		return guest_cr0();
	case 2:
		return vcpu->cr2;
	case 3:
		return vcpu->cr3;
	case 4:
		return guest_cr4();
	default:
		vcpu_printf(vcpu, "%s: unexpected cr %u\n", __FUNCTION__, cr);
		return 0;
	}
}

void realmode_set_cr(struct litevm_vcpu *vcpu, int cr, unsigned long val,
		     unsigned long *rflags)
{
	switch (cr) {
	case 0:
		set_cr0(vcpu, mk_cr_64(guest_cr0(), val));
		*rflags = vmcs_readl(GUEST_RFLAGS);
		break;
	case 2:
		vcpu->cr2 = val;
		break;
	case 3:
		set_cr3(vcpu, val);
		break;
	case 4:
		set_cr4(vcpu, mk_cr_64(guest_cr4(), val));
		break;
	default:
		vcpu_printf(vcpu, "%s: unexpected cr %u\n", __FUNCTION__, cr);
	}
}

static int handle_rmode_exception(struct litevm_vcpu *vcpu,
				  int vec, u32 err_code)
{
	if (!vcpu->rmode.active)
		return 0;

	if (vec == GP_VECTOR && err_code == 0)
		if (emulate_instruction(vcpu, 0, 0, 0) == EMULATE_DONE)
			return 1;
	return 0;
}

static int handle_exception(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	u32 intr_info, error_code;
	unsigned long cr2, rip;
	u32 vect_info;
	enum emulation_result er;

	vect_info = vmcs_read32(IDT_VECTORING_INFO_FIELD);
	intr_info = vmcs_read32(VM_EXIT_INTR_INFO);

	if ((vect_info & VECTORING_INFO_VALID_MASK) &&
						!is_page_fault(intr_info)) {
		printk(KERN_ERR "%s: unexpected, vectoring info 0x%x "
		       "intr info 0x%x\n", __FUNCTION__, vect_info, intr_info);
	}

	if (is_external_interrupt(vect_info)) {
		int irq = vect_info & VECTORING_INFO_VECTOR_MASK;
		set_bit(irq, vcpu->irq_pending);
		set_bit(irq / BITS_PER_LONG, &vcpu->irq_summary);
	}

	if ((intr_info & INTR_INFO_INTR_TYPE_MASK) == 0x200) { /* nmi */
		asm ("int $2");
		return 1;
	}
	error_code = 0;
	rip = vmcs_readl(GUEST_RIP);
	if (intr_info & INTR_INFO_DELIEVER_CODE_MASK)
		error_code = vmcs_read32(VM_EXIT_INTR_ERROR_CODE);
	if (is_page_fault(intr_info)) {
		cr2 = vmcs_readl(EXIT_QUALIFICATION);

		spin_lock(&vcpu->litevm->lock);
		if (!vcpu->mmu.page_fault(vcpu, cr2, error_code)) {
			spin_unlock(&vcpu->litevm->lock);
			return 1;
		}

		er = emulate_instruction(vcpu, litevm_run, cr2, error_code);
		spin_unlock(&vcpu->litevm->lock);

		switch (er) {
		case EMULATE_DONE:
			return 1;
		case EMULATE_DO_MMIO:
			++litevm_stat.mmio_exits;
			litevm_run->exit_reason = LITEVM_EXIT_MMIO;
			return 0;
		 case EMULATE_FAIL:
			vcpu_printf(vcpu, "%s: emulate fail\n", __FUNCTION__);
			break;
		default:
			BUG();
		}
	}

	if (vcpu->rmode.active &&
	    handle_rmode_exception(vcpu, intr_info & INTR_INFO_VECTOR_MASK,
								error_code))
		return 1;

	if ((intr_info & (INTR_INFO_INTR_TYPE_MASK | INTR_INFO_VECTOR_MASK)) == (INTR_TYPE_EXCEPTION | 1)) {
		litevm_run->exit_reason = LITEVM_EXIT_DEBUG;
		return 0;
	}
	litevm_run->exit_reason = LITEVM_EXIT_EXCEPTION;
	litevm_run->ex.exception = intr_info & INTR_INFO_VECTOR_MASK;
	litevm_run->ex.error_code = error_code;
	return 0;
}

static int handle_external_interrupt(struct litevm_vcpu *vcpu,
				     struct litevm_run *litevm_run)
{
	++litevm_stat.irq_exits;
	return 1;
}


static int get_io_count(struct litevm_vcpu *vcpu, u64 *count)
{
	u64 inst;
	gva_t rip;
	int countr_size;
	int i, n;

	if ((vmcs_readl(GUEST_RFLAGS) & X86_EFLAGS_VM)) {
		countr_size = 2;
	} else {
		u32 cs_ar = vmcs_read32(GUEST_CS_AR_BYTES);

		countr_size = (cs_ar & AR_L_MASK) ? 8:
			      (cs_ar & AR_DB_MASK) ? 4: 2;
	}

	rip =  vmcs_readl(GUEST_RIP);
	if (countr_size != 8)
		rip += vmcs_readl(GUEST_CS_BASE);

	n = litevm_read_guest(vcpu, rip, sizeof(inst), &inst);

	for (i = 0; i < n; i++) {
		switch (((u8*)&inst)[i]) {
		case 0xf0:
		case 0xf2:
		case 0xf3:
		case 0x2e:
		case 0x36:
		case 0x3e:
		case 0x26:
		case 0x64:
		case 0x65:
		case 0x66:
			break;
		case 0x67:
			countr_size = (countr_size == 2) ? 4: (countr_size >> 1);
		default:
			goto done;
		}
	}
	return 0;
done:
	countr_size *= 8;
	*count = vcpu->regs[VCPU_REGS_RCX] & (~0ULL >> (64 - countr_size));
	return 1;
}

static int handle_io(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	u64 exit_qualification;

	++litevm_stat.io_exits;
	exit_qualification = vmcs_read64(EXIT_QUALIFICATION);
	litevm_run->exit_reason = LITEVM_EXIT_IO;
	if (exit_qualification & 8)
		litevm_run->io.direction = LITEVM_EXIT_IO_IN;
	else
		litevm_run->io.direction = LITEVM_EXIT_IO_OUT;
	litevm_run->io.size = (exit_qualification & 7) + 1;
	litevm_run->io.string = (exit_qualification & 16) != 0;
	litevm_run->io.string_down
		= (vmcs_readl(GUEST_RFLAGS) & X86_EFLAGS_DF) != 0;
	litevm_run->io.rep = (exit_qualification & 32) != 0;
	litevm_run->io.port = exit_qualification >> 16;
	if (litevm_run->io.string) {
		if (!get_io_count(vcpu, &litevm_run->io.count))
			return 1;
		litevm_run->io.address = vmcs_readl(GUEST_LINEAR_ADDRESS);
	} else
		litevm_run->io.value = vcpu->regs[VCPU_REGS_RAX]; /* rax */
	return 0;
}

static int handle_invlpg(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	u64 address = vmcs_read64(EXIT_QUALIFICATION);
	int instruction_length = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	spin_lock(&vcpu->litevm->lock);
	vcpu->mmu.inval_page(vcpu, address);
	spin_unlock(&vcpu->litevm->lock);
	vmcs_writel(GUEST_RIP, vmcs_readl(GUEST_RIP) + instruction_length);
	return 1;
}

static int handle_cr(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	u64 exit_qualification;
	int cr;
	int reg;

#ifdef LITEVM_DEBUG
	if (guest_cpl() != 0) {
		vcpu_printf(vcpu, "%s: not supervisor\n", __FUNCTION__);
		inject_gp(vcpu);
		return 1;
	}
#endif

	exit_qualification = vmcs_read64(EXIT_QUALIFICATION);
	cr = exit_qualification & 15;
	reg = (exit_qualification >> 8) & 15;
	switch ((exit_qualification >> 4) & 3) {
	case 0: /* mov to cr */
		switch (cr) {
		case 0:
			vcpu_load_rsp_rip(vcpu);
			set_cr0(vcpu, vcpu->regs[reg]);
			skip_emulated_instruction(vcpu);
			return 1;
		case 3:
			vcpu_load_rsp_rip(vcpu);
			set_cr3(vcpu, vcpu->regs[reg]);
			skip_emulated_instruction(vcpu);
			return 1;
		case 4:
			vcpu_load_rsp_rip(vcpu);
			set_cr4(vcpu, vcpu->regs[reg]);
			skip_emulated_instruction(vcpu);
			return 1;
		case 8:
			vcpu_load_rsp_rip(vcpu);
			set_cr8(vcpu, vcpu->regs[reg]);
			skip_emulated_instruction(vcpu);
			return 1;
		};
		break;
	case 1: /*mov from cr*/
		switch (cr) {
		case 3:
			vcpu_load_rsp_rip(vcpu);
			vcpu->regs[reg] = vcpu->cr3;
			vcpu_put_rsp_rip(vcpu);
			skip_emulated_instruction(vcpu);
			return 1;
		case 8:
			printk(KERN_DEBUG "handle_cr: read CR8 "
			       "cpu erratum AA15\n");
			vcpu_load_rsp_rip(vcpu);
			vcpu->regs[reg] = vcpu->cr8;
			vcpu_put_rsp_rip(vcpu);
			skip_emulated_instruction(vcpu);
			return 1;
		}
		break;
	case 3: /* lmsw */
		lmsw(vcpu, (exit_qualification >> LMSW_SOURCE_DATA_SHIFT) & 0x0f);

		skip_emulated_instruction(vcpu);
		return 1;
	default:
		break;
	}
	litevm_run->exit_reason = 0;
	printk(KERN_ERR "litevm: unhandled control register: op %d cr %d\n",
	       (int)(exit_qualification >> 4) & 3, cr);
	return 0;
}

static int handle_dr(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	u64 exit_qualification;
	unsigned long val;
	int dr, reg;

	/*
	 * FIXME: this code assumes the host is debugging the guest.
	 *        need to deal with guest debugging itself too.
	 */
	exit_qualification = vmcs_read64(EXIT_QUALIFICATION);
	dr = exit_qualification & 7;
	reg = (exit_qualification >> 8) & 15;
	vcpu_load_rsp_rip(vcpu);
	if (exit_qualification & 16) {
		/* mov from dr */
		switch (dr) {
		case 6:
			val = 0xffff0ff0;
			break;
		case 7:
			val = 0x400;
			break;
		default:
			val = 0;
		}
		vcpu->regs[reg] = val;
	} else {
		/* mov to dr */
	}
	vcpu_put_rsp_rip(vcpu);
	skip_emulated_instruction(vcpu);
	return 1;
}

static int handle_cpuid(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	litevm_run->exit_reason = LITEVM_EXIT_CPUID;
	return 0;
}

static int handle_rdmsr(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	u32 ecx = vcpu->regs[VCPU_REGS_RCX];
	struct vmx_msr_entry *msr = find_msr_entry(vcpu, ecx);
	u64 data;

#ifdef LITEVM_DEBUG
	if (guest_cpl() != 0) {
		vcpu_printf(vcpu, "%s: not supervisor\n", __FUNCTION__);
		inject_gp(vcpu);
		return 1;
	}
#endif

	switch (ecx) {
#ifdef __x86_64__
	case MSR_FS_BASE:
		data = vmcs_readl(GUEST_FS_BASE);
		break;
	case MSR_GS_BASE:
		data = vmcs_readl(GUEST_GS_BASE);
		break;
#endif
	case MSR_IA32_SYSENTER_CS:
		data = vmcs_read32(GUEST_SYSENTER_CS);
		break;
	case MSR_IA32_SYSENTER_EIP:
		data = vmcs_read32(GUEST_SYSENTER_EIP);
		break;
	case MSR_IA32_SYSENTER_ESP:
		data = vmcs_read32(GUEST_SYSENTER_ESP);
		break;
	case MSR_IA32_MC0_CTL:
	case MSR_IA32_MCG_STATUS:
	case MSR_IA32_MCG_CAP:
	case MSR_IA32_MC0_MISC:
	case MSR_IA32_MC0_MISC+4:
	case MSR_IA32_MC0_MISC+8:
	case MSR_IA32_MC0_MISC+12:
	case MSR_IA32_MC0_MISC+16:
	case MSR_IA32_UCODE_REV:
		/* MTRR registers */
	case 0xfe:
	case 0x200 ... 0x2ff:
		data = 0;
		break;
	case MSR_IA32_APICBASE:
		data = vcpu->apic_base;
		break;
	default:
		if (msr) {
			data = msr->data;
			break;
		}
		printk(KERN_ERR "litevm: unhandled rdmsr: %x\n", ecx);
		inject_gp(vcpu);
		return 1;
	}

	/* FIXME: handling of bits 32:63 of rax, rdx */
	vcpu->regs[VCPU_REGS_RAX] = data & -1u;
	vcpu->regs[VCPU_REGS_RDX] = (data >> 32) & -1u;
	skip_emulated_instruction(vcpu);
	return 1;
}

#ifdef __x86_64__

static void set_efer(struct litevm_vcpu *vcpu, u64 efer)
{
	struct vmx_msr_entry *msr;

	if (efer & EFER_RESERVED_BITS) {
		printk(KERN_DEBUG "set_efer: 0x%llx #GP, reserved bits\n",
		       efer);
		inject_gp(vcpu);
		return;
	}

	if (is_paging() && (vcpu->shadow_efer & EFER_LME) != (efer & EFER_LME)) {
		printk(KERN_DEBUG "set_efer: #GP, change LME while paging\n");
		inject_gp(vcpu);
		return;
	}

	efer &= ~EFER_LMA;
	efer |= vcpu->shadow_efer & EFER_LMA;

	vcpu->shadow_efer = efer;

	msr = find_msr_entry(vcpu, MSR_EFER);

	if (!(efer & EFER_LMA))
	    efer &= ~EFER_LME;
	msr->data = efer;
	skip_emulated_instruction(vcpu);
}

#endif

#define MSR_IA32_TIME_STAMP_COUNTER 0x10

static int handle_wrmsr(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	u32 ecx = vcpu->regs[VCPU_REGS_RCX];
	struct vmx_msr_entry *msr;
	u64 data = (vcpu->regs[VCPU_REGS_RAX] & -1u)
		| ((u64)(vcpu->regs[VCPU_REGS_RDX] & -1u) << 32);

#ifdef LITEVM_DEBUG
	if (guest_cpl() != 0) {
		vcpu_printf(vcpu, "%s: not supervisor\n", __FUNCTION__);
		inject_gp(vcpu);
		return 1;
	}
#endif

	switch (ecx) {
#ifdef __x86_64__
	case MSR_FS_BASE:
		vmcs_writel(GUEST_FS_BASE, data);
		break;
	case MSR_GS_BASE:
		vmcs_writel(GUEST_GS_BASE, data);
		break;
#endif
	case MSR_IA32_SYSENTER_CS:
		vmcs_write32(GUEST_SYSENTER_CS, data);
		break;
	case MSR_IA32_SYSENTER_EIP:
		vmcs_write32(GUEST_SYSENTER_EIP, data);
		break;
	case MSR_IA32_SYSENTER_ESP:
		vmcs_write32(GUEST_SYSENTER_ESP, data);
		break;
#ifdef __x86_64
	case MSR_EFER:
		set_efer(vcpu, data);
		return 1;
	case MSR_IA32_MC0_STATUS:
		printk(KERN_WARNING "%s: MSR_IA32_MC0_STATUS 0x%llx, nop\n"
			    , __FUNCTION__, data);
		break;
#endif
	case MSR_IA32_TIME_STAMP_COUNTER: {
		u64 tsc;

		rdtscll(tsc);
		vmcs_write64(TSC_OFFSET, data - tsc);
		break;
	}
	case MSR_IA32_UCODE_REV:
	case MSR_IA32_UCODE_WRITE:
	case 0x200 ... 0x2ff: /* MTRRs */
		break;
	case MSR_IA32_APICBASE:
		vcpu->apic_base = data;
		break;
	default:
		msr = find_msr_entry(vcpu, ecx);
		if (msr) {
			msr->data = data;
			break;
		}
		printk(KERN_ERR "litevm: unhandled wrmsr: %x\n", ecx);
		inject_gp(vcpu);
		return 1;
	}
	skip_emulated_instruction(vcpu);
	return 1;
}

static int handle_interrupt_window(struct litevm_vcpu *vcpu,
				   struct litevm_run *litevm_run)
{
	/* Turn off interrupt window reporting. */
	vmcs_write32(CPU_BASED_VM_EXEC_CONTROL,
		     vmcs_read32(CPU_BASED_VM_EXEC_CONTROL)
		     & ~CPU_BASED_VIRTUAL_INTR_PENDING);
	return 1;
}

static int handle_halt(struct litevm_vcpu *vcpu, struct litevm_run *litevm_run)
{
	skip_emulated_instruction(vcpu);
	if (vcpu->irq_summary && (vmcs_readl(GUEST_RFLAGS) & X86_EFLAGS_IF))
		return 1;

	litevm_run->exit_reason = LITEVM_EXIT_HLT;
	return 0;
}

/*
 * The exit handlers return 1 if the exit was handled fully and guest execution
 * may resume.  Otherwise they set the litevm_run parameter to indicate what needs
 * to be done to userspace and return 0.
 */
static int (*litevm_vmx_exit_handlers[])(struct litevm_vcpu *vcpu,
				      struct litevm_run *litevm_run) = {
	[EXIT_REASON_EXCEPTION_NMI]           = handle_exception,
	[EXIT_REASON_EXTERNAL_INTERRUPT]      = handle_external_interrupt,
	[EXIT_REASON_IO_INSTRUCTION]          = handle_io,
	[EXIT_REASON_INVLPG]                  = handle_invlpg,
	[EXIT_REASON_CR_ACCESS]               = handle_cr,
	[EXIT_REASON_DR_ACCESS]               = handle_dr,
	[EXIT_REASON_CPUID]                   = handle_cpuid,
	[EXIT_REASON_MSR_READ]                = handle_rdmsr,
	[EXIT_REASON_MSR_WRITE]               = handle_wrmsr,
	[EXIT_REASON_PENDING_INTERRUPT]       = handle_interrupt_window,
	[EXIT_REASON_HLT]                     = handle_halt,
};

static const int litevm_vmx_max_exit_handlers =
	sizeof(litevm_vmx_exit_handlers) / sizeof(*litevm_vmx_exit_handlers);

/*
 * The guest has exited.  See if we can fix it or if we need userspace
 * assistance.
 */
static int litevm_handle_exit(struct litevm_run *litevm_run, struct litevm_vcpu *vcpu)
{
	u32 vectoring_info = vmcs_read32(IDT_VECTORING_INFO_FIELD);
	u32 exit_reason = vmcs_read32(VM_EXIT_REASON);

	if ( (vectoring_info & VECTORING_INFO_VALID_MASK) &&
				exit_reason != EXIT_REASON_EXCEPTION_NMI )
		printk(KERN_WARNING "%s: unexpected, valid vectoring info and "
		       "exit reason is 0x%x\n", __FUNCTION__, exit_reason);
	litevm_run->instruction_length = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
	if (exit_reason < litevm_vmx_max_exit_handlers
	    && litevm_vmx_exit_handlers[exit_reason])
		return litevm_vmx_exit_handlers[exit_reason](vcpu, litevm_run);
	else {
		litevm_run->exit_reason = LITEVM_EXIT_UNKNOWN;
		litevm_run->hw.hardware_exit_reason = exit_reason;
	}
	return 0;
}

static void inject_rmode_irq(struct litevm_vcpu *vcpu, int irq)
{
	u16 ent[2];
	u16 cs;
	u16 ip;
	unsigned long flags;
	unsigned long ss_base = vmcs_readl(GUEST_SS_BASE);
	u16 sp =  vmcs_readl(GUEST_RSP);
	u32 ss_limit = vmcs_read32(GUEST_SS_LIMIT);

	if (sp > ss_limit || sp - 6 > sp) {
		vcpu_printf(vcpu, "%s: #SS, rsp 0x%lx ss 0x%lx limit 0x%x\n",
			    __FUNCTION__,
			    vmcs_readl(GUEST_RSP),
			    vmcs_readl(GUEST_SS_BASE),
			    vmcs_read32(GUEST_SS_LIMIT));
		return;
	}

	if (litevm_read_guest(vcpu, irq * sizeof(ent), sizeof(ent), &ent) !=
								sizeof(ent)) {
		vcpu_printf(vcpu, "%s: read guest err\n", __FUNCTION__);
		return;
	}

	flags =  vmcs_readl(GUEST_RFLAGS);
	cs =  vmcs_readl(GUEST_CS_BASE) >> 4;
	ip =  vmcs_readl(GUEST_RIP);


	if (litevm_write_guest(vcpu, ss_base + sp - 2, 2, &flags) != 2 ||
	    litevm_write_guest(vcpu, ss_base + sp - 4, 2, &cs) != 2 ||
	    litevm_write_guest(vcpu, ss_base + sp - 6, 2, &ip) != 2) {
		vcpu_printf(vcpu, "%s: write guest err\n", __FUNCTION__);
		return;
	}

	vmcs_writel(GUEST_RFLAGS, flags &
		    ~( X86_EFLAGS_IF | X86_EFLAGS_AC | X86_EFLAGS_TF));
	vmcs_write16(GUEST_CS_SELECTOR, ent[1]) ;
	vmcs_writel(GUEST_CS_BASE, ent[1] << 4);
	vmcs_writel(GUEST_RIP, ent[0]);
	vmcs_writel(GUEST_RSP, (vmcs_readl(GUEST_RSP) & ~0xffff) | (sp - 6));
}

static void litevm_do_inject_irq(struct litevm_vcpu *vcpu)
{
	int word_index = __ffs(vcpu->irq_summary);
	int bit_index = __ffs(vcpu->irq_pending[word_index]);
	int irq = word_index * BITS_PER_LONG + bit_index;

	clear_bit(bit_index, &vcpu->irq_pending[word_index]);
	if (!vcpu->irq_pending[word_index])
		clear_bit(word_index, &vcpu->irq_summary);

	if (vcpu->rmode.active) {
		inject_rmode_irq(vcpu, irq);
		return;
	}
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD,
			irq | INTR_TYPE_EXT_INTR | INTR_INFO_VALID_MASK);
}

static void litevm_try_inject_irq(struct litevm_vcpu *vcpu)
{
	if ((vmcs_readl(GUEST_RFLAGS) & X86_EFLAGS_IF)
	    && (vmcs_read32(GUEST_INTERRUPTIBILITY_INFO) & 3) == 0)
		/*
		 * Interrupts enabled, and not blocked by sti or mov ss. Good.
		 */
		litevm_do_inject_irq(vcpu);
	else
		/*
		 * Interrupts blocked.  Wait for unblock.
		 */
		vmcs_write32(CPU_BASED_VM_EXEC_CONTROL,
			     vmcs_read32(CPU_BASED_VM_EXEC_CONTROL)
			     | CPU_BASED_VIRTUAL_INTR_PENDING);
}

static void litevm_guest_debug_pre(struct litevm_vcpu *vcpu)
{
	struct litevm_guest_debug *dbg = &vcpu->guest_debug;

	set_debugreg(dbg->bp[0], 0);
	set_debugreg(dbg->bp[1], 1);
	set_debugreg(dbg->bp[2], 2);
	set_debugreg(dbg->bp[3], 3);

	if (dbg->singlestep) {
		unsigned long flags;

		flags = vmcs_readl(GUEST_RFLAGS);
		flags |= X86_EFLAGS_TF | X86_EFLAGS_RF;
		vmcs_writel(GUEST_RFLAGS, flags);
	}
}

static void load_msrs(struct vmx_msr_entry *e, int n)
{
	int i;

	for (i = 0; i < n; ++i)
		wrmsrl(e[i].index, e[i].data);
}

static void save_msrs(struct vmx_msr_entry *e, int n)
{
	int i;

	for (i = 0; i < n; ++i)
		rdmsrl(e[i].index, e[i].data);
}

static int litevm_dev_ioctl_run(struct litevm *litevm, struct litevm_run *litevm_run)
{
	struct litevm_vcpu *vcpu;
	u8 fail;
	u16 fs_sel, gs_sel, ldt_sel;
	int fs_gs_ldt_reload_needed;

	if (litevm_run->vcpu < 0 || litevm_run->vcpu >= LITEVM_MAX_VCPUS)
		return -EINVAL;

	vcpu = vcpu_load(litevm, litevm_run->vcpu);
	if (!vcpu)
		return -ENOENT;

	if (litevm_run->emulated) {
		skip_emulated_instruction(vcpu);
		litevm_run->emulated = 0;
	}

	if (litevm_run->mmio_completed) {
		memcpy(vcpu->mmio_data, litevm_run->mmio.data, 8);
		vcpu->mmio_read_completed = 1;
	}

	vcpu->mmio_needed = 0;

again:
	/*
	 * Set host fs and gs selectors.  Unfortunately, 22.2.3 does not
	 * allow segment selectors with cpl > 0 or ti == 1.
	 */
	fs_sel = read_fs();
	gs_sel = read_gs();
	ldt_sel = read_ldt();
	fs_gs_ldt_reload_needed = (fs_sel & 7) | (gs_sel & 7) | ldt_sel;
	if (!fs_gs_ldt_reload_needed) {
		vmcs_write16(HOST_FS_SELECTOR, fs_sel);
		vmcs_write16(HOST_GS_SELECTOR, gs_sel);
	} else {
		vmcs_write16(HOST_FS_SELECTOR, 0);
		vmcs_write16(HOST_GS_SELECTOR, 0);
	}

#ifdef __x86_64__
	vmcs_writel(HOST_FS_BASE, read_msr(MSR_FS_BASE));
	vmcs_writel(HOST_GS_BASE, read_msr(MSR_GS_BASE));
#endif

	if (vcpu->irq_summary &&
	    !(vmcs_read32(VM_ENTRY_INTR_INFO_FIELD) & INTR_INFO_VALID_MASK))
		litevm_try_inject_irq(vcpu);

	if (vcpu->guest_debug.enabled)
		litevm_guest_debug_pre(vcpu);

	fx_save(vcpu->host_fx_image);
	fx_restore(vcpu->guest_fx_image);

	save_msrs(vcpu->host_msrs, vcpu->nmsrs);
	load_msrs(vcpu->guest_msrs, NR_BAD_MSRS);

	asm (
		/* Store host registers */
		"pushf \n\t"
#ifdef __x86_64__
		"push %%rax; push %%rbx; push %%rdx;"
		"push %%rsi; push %%rdi; push %%rbp;"
		"push %%r8;  push %%r9;  push %%r10; push %%r11;"
		"push %%r12; push %%r13; push %%r14; push %%r15;"
		"push %%rcx \n\t"
		"vmwrite %%rsp, %2 \n\t"
#else
		"pusha; push %%ecx \n\t"
		"vmwrite %%esp, %2 \n\t"
#endif
		/* Check if vmlaunch of vmresume is needed */
		"cmp $0, %1 \n\t"
		/* Load guest registers.  Don't clobber flags. */
#ifdef __x86_64__
		"mov %c[cr2](%3), %%rax \n\t"
		"mov %%rax, %%cr2 \n\t"
		"mov %c[rax](%3), %%rax \n\t"
		"mov %c[rbx](%3), %%rbx \n\t"
		"mov %c[rdx](%3), %%rdx \n\t"
		"mov %c[rsi](%3), %%rsi \n\t"
		"mov %c[rdi](%3), %%rdi \n\t"
		"mov %c[rbp](%3), %%rbp \n\t"
		"mov %c[r8](%3),  %%r8  \n\t"
		"mov %c[r9](%3),  %%r9  \n\t"
		"mov %c[r10](%3), %%r10 \n\t"
		"mov %c[r11](%3), %%r11 \n\t"
		"mov %c[r12](%3), %%r12 \n\t"
		"mov %c[r13](%3), %%r13 \n\t"
		"mov %c[r14](%3), %%r14 \n\t"
		"mov %c[r15](%3), %%r15 \n\t"
		"mov %c[rcx](%3), %%rcx \n\t" /* kills %3 (rcx) */
#else
		"mov %c[cr2](%3), %%eax \n\t"
		"mov %%eax,   %%cr2 \n\t"
		"mov %c[rax](%3), %%eax \n\t"
		"mov %c[rbx](%3), %%ebx \n\t"
		"mov %c[rdx](%3), %%edx \n\t"
		"mov %c[rsi](%3), %%esi \n\t"
		"mov %c[rdi](%3), %%edi \n\t"
		"mov %c[rbp](%3), %%ebp \n\t"
		"mov %c[rcx](%3), %%ecx \n\t" /* kills %3 (ecx) */
#endif
		/* Enter guest mode */
		"jne launched \n\t"
		"vmlaunch \n\t"
		"jmp litevm_vmx_return \n\t"
		"launched: vmresume \n\t"
		".globl litevm_vmx_return \n\t"
		"litevm_vmx_return: "
		/* Save guest registers, load host registers, keep flags */
#ifdef __x86_64__
		"xchg %3,     0(%%rsp) \n\t"
		"mov %%rax, %c[rax](%3) \n\t"
		"mov %%rbx, %c[rbx](%3) \n\t"
		"pushq 0(%%rsp); popq %c[rcx](%3) \n\t"
		"mov %%rdx, %c[rdx](%3) \n\t"
		"mov %%rsi, %c[rsi](%3) \n\t"
		"mov %%rdi, %c[rdi](%3) \n\t"
		"mov %%rbp, %c[rbp](%3) \n\t"
		"mov %%r8,  %c[r8](%3) \n\t"
		"mov %%r9,  %c[r9](%3) \n\t"
		"mov %%r10, %c[r10](%3) \n\t"
		"mov %%r11, %c[r11](%3) \n\t"
		"mov %%r12, %c[r12](%3) \n\t"
		"mov %%r13, %c[r13](%3) \n\t"
		"mov %%r14, %c[r14](%3) \n\t"
		"mov %%r15, %c[r15](%3) \n\t"
		"mov %%cr2, %%rax   \n\t"
		"mov %%rax, %c[cr2](%3) \n\t"
		"mov 0(%%rsp), %3 \n\t"

		"pop  %%rcx; pop  %%r15; pop  %%r14; pop  %%r13; pop  %%r12;"
		"pop  %%r11; pop  %%r10; pop  %%r9;  pop  %%r8;"
		"pop  %%rbp; pop  %%rdi; pop  %%rsi;"
		"pop  %%rdx; pop  %%rbx; pop  %%rax \n\t"
#else
		"xchg %3, 0(%%esp) \n\t"
		"mov %%eax, %c[rax](%3) \n\t"
		"mov %%ebx, %c[rbx](%3) \n\t"
		"pushl 0(%%esp); popl %c[rcx](%3) \n\t"
		"mov %%edx, %c[rdx](%3) \n\t"
		"mov %%esi, %c[rsi](%3) \n\t"
		"mov %%edi, %c[rdi](%3) \n\t"
		"mov %%ebp, %c[rbp](%3) \n\t"
		"mov %%cr2, %%eax  \n\t"
		"mov %%eax, %c[cr2](%3) \n\t"
		"mov 0(%%esp), %3 \n\t"

		"pop %%ecx; popa \n\t"
#endif
		"setbe %0 \n\t"
		"popf \n\t"
	      : "=g" (fail)
	      : "r"(vcpu->launched), "r"((unsigned long)HOST_RSP),
		"c"(vcpu),
		[rax]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_RAX])),
		[rbx]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_RBX])),
		[rcx]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_RCX])),
		[rdx]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_RDX])),
		[rsi]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_RSI])),
		[rdi]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_RDI])),
		[rbp]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_RBP])),
#ifdef __x86_64__
		[r8 ]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R8 ])),
		[r9 ]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R9 ])),
		[r10]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R10])),
		[r11]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R11])),
		[r12]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R12])),
		[r13]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R13])),
		[r14]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R14])),
		[r15]"i"(offsetof(struct litevm_vcpu, regs[VCPU_REGS_R15])),
#endif
		[cr2]"i"(offsetof(struct litevm_vcpu, cr2))
	      : "cc", "memory" );

	++litevm_stat.exits;

	save_msrs(vcpu->guest_msrs, NR_BAD_MSRS);
	load_msrs(vcpu->host_msrs, NR_BAD_MSRS);

	fx_save(vcpu->guest_fx_image);
	fx_restore(vcpu->host_fx_image);

#ifndef __x86_64__
	asm ("mov %0, %%ds; mov %0, %%es" : : "r"(__USER_DS));
#endif

	litevm_run->exit_type = 0;
	if (fail) {
		litevm_run->exit_type = LITEVM_EXIT_TYPE_FAIL_ENTRY;
		litevm_run->exit_reason = vmcs_read32(VM_INSTRUCTION_ERROR);
	} else {
		if (fs_gs_ldt_reload_needed) {
			load_ldt(ldt_sel);
			load_fs(fs_sel);
			/*
			 * If we have to reload gs, we must take care to
			 * preserve our gs base.
			 */
			local_irq_disable();
			load_gs(gs_sel);
#ifdef __x86_64__
			wrmsrl(MSR_GS_BASE, vmcs_readl(HOST_GS_BASE));
#endif
			local_irq_enable();

			reload_tss();
		}
		vcpu->launched = 1;
		litevm_run->exit_type = LITEVM_EXIT_TYPE_VM_EXIT;
		if (litevm_handle_exit(litevm_run, vcpu)) {
			/* Give scheduler a change to reschedule. */
			vcpu_put(vcpu);
			if (signal_pending(current)) {
				++litevm_stat.signal_exits;
				return -EINTR;
			}
			cond_resched();
			/* Cannot fail -  no vcpu unplug yet. */
			vcpu_load(litevm, vcpu_slot(vcpu));
			goto again;
		}
	}

	vcpu_put(vcpu);
	return 0;
}

static int litevm_dev_ioctl_get_regs(struct litevm *litevm, struct litevm_regs *regs)
{
	struct litevm_vcpu *vcpu;

	if (regs->vcpu < 0 || regs->vcpu >= LITEVM_MAX_VCPUS)
		return -EINVAL;

	vcpu = vcpu_load(litevm, regs->vcpu);
	if (!vcpu)
		return -ENOENT;

	regs->rax = vcpu->regs[VCPU_REGS_RAX];
	regs->rbx = vcpu->regs[VCPU_REGS_RBX];
	regs->rcx = vcpu->regs[VCPU_REGS_RCX];
	regs->rdx = vcpu->regs[VCPU_REGS_RDX];
	regs->rsi = vcpu->regs[VCPU_REGS_RSI];
	regs->rdi = vcpu->regs[VCPU_REGS_RDI];
	regs->rsp = vmcs_readl(GUEST_RSP);
	regs->rbp = vcpu->regs[VCPU_REGS_RBP];
#ifdef __x86_64__
	regs->r8 = vcpu->regs[VCPU_REGS_R8];
	regs->r9 = vcpu->regs[VCPU_REGS_R9];
	regs->r10 = vcpu->regs[VCPU_REGS_R10];
	regs->r11 = vcpu->regs[VCPU_REGS_R11];
	regs->r12 = vcpu->regs[VCPU_REGS_R12];
	regs->r13 = vcpu->regs[VCPU_REGS_R13];
	regs->r14 = vcpu->regs[VCPU_REGS_R14];
	regs->r15 = vcpu->regs[VCPU_REGS_R15];
#endif

	regs->rip = vmcs_readl(GUEST_RIP);
	regs->rflags = vmcs_readl(GUEST_RFLAGS);

	/*
	 * Don't leak debug flags in case they were set for guest debugging
	 */
	if (vcpu->guest_debug.enabled && vcpu->guest_debug.singlestep)
		regs->rflags &= ~(X86_EFLAGS_TF | X86_EFLAGS_RF);

	vcpu_put(vcpu);

	return 0;
}

static int litevm_dev_ioctl_set_regs(struct litevm *litevm, struct litevm_regs *regs)
{
	struct litevm_vcpu *vcpu;

	if (regs->vcpu < 0 || regs->vcpu >= LITEVM_MAX_VCPUS)
		return -EINVAL;

	vcpu = vcpu_load(litevm, regs->vcpu);
	if (!vcpu)
		return -ENOENT;

	vcpu->regs[VCPU_REGS_RAX] = regs->rax;
	vcpu->regs[VCPU_REGS_RBX] = regs->rbx;
	vcpu->regs[VCPU_REGS_RCX] = regs->rcx;
	vcpu->regs[VCPU_REGS_RDX] = regs->rdx;
	vcpu->regs[VCPU_REGS_RSI] = regs->rsi;
	vcpu->regs[VCPU_REGS_RDI] = regs->rdi;
	vmcs_writel(GUEST_RSP, regs->rsp);
	vcpu->regs[VCPU_REGS_RBP] = regs->rbp;
#ifdef __x86_64__
	vcpu->regs[VCPU_REGS_R8] = regs->r8;
	vcpu->regs[VCPU_REGS_R9] = regs->r9;
	vcpu->regs[VCPU_REGS_R10] = regs->r10;
	vcpu->regs[VCPU_REGS_R11] = regs->r11;
	vcpu->regs[VCPU_REGS_R12] = regs->r12;
	vcpu->regs[VCPU_REGS_R13] = regs->r13;
	vcpu->regs[VCPU_REGS_R14] = regs->r14;
	vcpu->regs[VCPU_REGS_R15] = regs->r15;
#endif

	vmcs_writel(GUEST_RIP, regs->rip);
	vmcs_writel(GUEST_RFLAGS, regs->rflags);

	vcpu_put(vcpu);

	return 0;
}

static int litevm_dev_ioctl_get_sregs(struct litevm *litevm, struct litevm_sregs *sregs)
{
	struct litevm_vcpu *vcpu;

	if (sregs->vcpu < 0 || sregs->vcpu >= LITEVM_MAX_VCPUS)
		return -EINVAL;
	vcpu = vcpu_load(litevm, sregs->vcpu);
	if (!vcpu)
		return -ENOENT;

#define get_segment(var, seg) \
	do { \
		u32 ar; \
		\
		sregs->var.base = vmcs_readl(GUEST_##seg##_BASE); \
		sregs->var.limit = vmcs_read32(GUEST_##seg##_LIMIT); \
		sregs->var.selector = vmcs_read16(GUEST_##seg##_SELECTOR); \
		ar = vmcs_read32(GUEST_##seg##_AR_BYTES); \
		if (ar & AR_UNUSABLE_MASK) ar = 0; \
		sregs->var.type = ar & 15; \
		sregs->var.s = (ar >> 4) & 1; \
		sregs->var.dpl = (ar >> 5) & 3; \
		sregs->var.present = (ar >> 7) & 1; \
		sregs->var.avl = (ar >> 12) & 1; \
		sregs->var.l = (ar >> 13) & 1; \
		sregs->var.db = (ar >> 14) & 1; \
		sregs->var.g = (ar >> 15) & 1; \
		sregs->var.unusable = (ar >> 16) & 1; \
	} while (0);

	get_segment(cs, CS);
	get_segment(ds, DS);
	get_segment(es, ES);
	get_segment(fs, FS);
	get_segment(gs, GS);
	get_segment(ss, SS);

	get_segment(tr, TR);
	get_segment(ldt, LDTR);
#undef get_segment

#define get_dtable(var, table) \
	sregs->var.limit = vmcs_read32(GUEST_##table##_LIMIT), \
		sregs->var.base = vmcs_readl(GUEST_##table##_BASE)

	get_dtable(idt, IDTR);
	get_dtable(gdt, GDTR);
#undef get_dtable

	sregs->cr0 = guest_cr0();
	sregs->cr2 = vcpu->cr2;
	sregs->cr3 = vcpu->cr3;
	sregs->cr4 = guest_cr4();
	sregs->cr8 = vcpu->cr8;
	sregs->efer = vcpu->shadow_efer;
	sregs->apic_base = vcpu->apic_base;

	sregs->pending_int = vcpu->irq_summary != 0;

	vcpu_put(vcpu);

	return 0;
}

static int litevm_dev_ioctl_set_sregs(struct litevm *litevm, struct litevm_sregs *sregs)
{
	struct litevm_vcpu *vcpu;
	int mmu_reset_needed = 0;

	if (sregs->vcpu < 0 || sregs->vcpu >= LITEVM_MAX_VCPUS)
		return -EINVAL;
	vcpu = vcpu_load(litevm, sregs->vcpu);
	if (!vcpu)
		return -ENOENT;

#define set_segment(var, seg) \
	do { \
		u32 ar; \
		\
		vmcs_writel(GUEST_##seg##_BASE, sregs->var.base);  \
		vmcs_write32(GUEST_##seg##_LIMIT, sregs->var.limit); \
		vmcs_write16(GUEST_##seg##_SELECTOR, sregs->var.selector); \
		if (sregs->var.unusable) { \
			ar = (1 << 16); \
		} else { \
			ar = (sregs->var.type & 15); \
			ar |= (sregs->var.s & 1) << 4; \
			ar |= (sregs->var.dpl & 3) << 5; \
			ar |= (sregs->var.present & 1) << 7; \
			ar |= (sregs->var.avl & 1) << 12; \
			ar |= (sregs->var.l & 1) << 13; \
			ar |= (sregs->var.db & 1) << 14; \
			ar |= (sregs->var.g & 1) << 15; \
		} \
		vmcs_write32(GUEST_##seg##_AR_BYTES, ar); \
	} while (0);

	set_segment(cs, CS);
	set_segment(ds, DS);
	set_segment(es, ES);
	set_segment(fs, FS);
	set_segment(gs, GS);
	set_segment(ss, SS);

	set_segment(tr, TR);

	set_segment(ldt, LDTR);
#undef set_segment

#define set_dtable(var, table) \
	vmcs_write32(GUEST_##table##_LIMIT, sregs->var.limit), \
	vmcs_writel(GUEST_##table##_BASE, sregs->var.base)

	set_dtable(idt, IDTR);
	set_dtable(gdt, GDTR);
#undef set_dtable

	vcpu->cr2 = sregs->cr2;
	mmu_reset_needed |= vcpu->cr3 != sregs->cr3;
	vcpu->cr3 = sregs->cr3;

	vcpu->cr8 = sregs->cr8;

	mmu_reset_needed |= vcpu->shadow_efer != sregs->efer;
#ifdef __x86_64__
	__set_efer(vcpu, sregs->efer);
#endif
	vcpu->apic_base = sregs->apic_base;

	mmu_reset_needed |= guest_cr0() != sregs->cr0;
	vcpu->rmode.active = ((sregs->cr0 & CR0_PE_MASK) == 0);
	update_exception_bitmap(vcpu);
	vmcs_writel(CR0_READ_SHADOW, sregs->cr0);
	vmcs_writel(GUEST_CR0, sregs->cr0 | LITEVM_VM_CR0_ALWAYS_ON);

	mmu_reset_needed |=  guest_cr4() != sregs->cr4;
	__set_cr4(vcpu, sregs->cr4);

	if (mmu_reset_needed)
		litevm_mmu_reset_context(vcpu);
	vcpu_put(vcpu);

	return 0;
}

/*
 * Translate a guest virtual address to a guest physical address.
 */
static int litevm_dev_ioctl_translate(struct litevm *litevm, struct litevm_translation *tr)
{
	unsigned long vaddr = tr->linear_address;
	struct litevm_vcpu *vcpu;
	gpa_t gpa;

	vcpu = vcpu_load(litevm, tr->vcpu);
	if (!vcpu)
		return -ENOENT;
	spin_lock(&litevm->lock);
	gpa = vcpu->mmu.gva_to_gpa(vcpu, vaddr);
	tr->physical_address = gpa;
	tr->valid = gpa != UNMAPPED_GVA;
	tr->writeable = 1;
	tr->usermode = 0;
	spin_unlock(&litevm->lock);
	vcpu_put(vcpu);

	return 0;
}

static int litevm_dev_ioctl_interrupt(struct litevm *litevm, struct litevm_interrupt *irq)
{
	struct litevm_vcpu *vcpu;

	if (irq->vcpu < 0 || irq->vcpu >= LITEVM_MAX_VCPUS)
		return -EINVAL;
	if (irq->irq < 0 || irq->irq >= 256)
		return -EINVAL;
	vcpu = vcpu_load(litevm, irq->vcpu);
	if (!vcpu)
		return -ENOENT;

	set_bit(irq->irq, vcpu->irq_pending);
	set_bit(irq->irq / BITS_PER_LONG, &vcpu->irq_summary);

	vcpu_put(vcpu);

	return 0;
}

static int litevm_dev_ioctl_debug_guest(struct litevm *litevm,
				     struct litevm_debug_guest *dbg)
{
	struct litevm_vcpu *vcpu;
	unsigned long dr7 = 0x400;
	u32 exception_bitmap;
	int old_singlestep;

	if (dbg->vcpu < 0 || dbg->vcpu >= LITEVM_MAX_VCPUS)
		return -EINVAL;
	vcpu = vcpu_load(litevm, dbg->vcpu);
	if (!vcpu)
		return -ENOENT;

	exception_bitmap = vmcs_read32(EXCEPTION_BITMAP);
	old_singlestep = vcpu->guest_debug.singlestep;

	vcpu->guest_debug.enabled = dbg->enabled;
	if (vcpu->guest_debug.enabled) {
		int i;

		dr7 |= 0x200;  /* exact */
		for (i = 0; i < 4; ++i) {
			if (!dbg->breakpoints[i].enabled)
				continue;
			vcpu->guest_debug.bp[i] = dbg->breakpoints[i].address;
			dr7 |= 2 << (i*2);    /* global enable */
			dr7 |= 0 << (i*4+16); /* execution breakpoint */
		}

		exception_bitmap |= (1u << 1);  /* Trap debug exceptions */

		vcpu->guest_debug.singlestep = dbg->singlestep;
	} else {
		exception_bitmap &= ~(1u << 1); /* Ignore debug exceptions */
		vcpu->guest_debug.singlestep = 0;
	}

	if (old_singlestep && !vcpu->guest_debug.singlestep) {
		unsigned long flags;

		flags = vmcs_readl(GUEST_RFLAGS);
		flags &= ~(X86_EFLAGS_TF | X86_EFLAGS_RF);
		vmcs_writel(GUEST_RFLAGS, flags);
	}

	vmcs_write32(EXCEPTION_BITMAP, exception_bitmap);
	vmcs_writel(GUEST_DR7, dr7);

	vcpu_put(vcpu);

	return 0;
}

static long litevm_dev_ioctl(struct file *filp,
			  unsigned int ioctl, unsigned long arg)
{
	struct litevm *litevm = filp->private_data;
	int r = -EINVAL;

	switch (ioctl) {
	case LITEVM_CREATE_VCPU: {
		r = litevm_dev_ioctl_create_vcpu(litevm, arg);
		if (r)
			goto out;
		break;
	}
	case LITEVM_RUN: {
		struct litevm_run litevm_run;

		r = -EFAULT;
		if (copy_from_user(&litevm_run, (void *)arg, sizeof litevm_run))
			goto out;
		r = litevm_dev_ioctl_run(litevm, &litevm_run);
		if (r < 0)
			goto out;
		r = -EFAULT;
		if (copy_to_user((void *)arg, &litevm_run, sizeof litevm_run))
			goto out;
		r = 0;
		break;
	}
	case LITEVM_GET_REGS: {
		struct litevm_regs litevm_regs;

		r = -EFAULT;
		if (copy_from_user(&litevm_regs, (void *)arg, sizeof litevm_regs))
			goto out;
		r = litevm_dev_ioctl_get_regs(litevm, &litevm_regs);
		if (r)
			goto out;
		r = -EFAULT;
		if (copy_to_user((void *)arg, &litevm_regs, sizeof litevm_regs))
			goto out;
		r = 0;
		break;
	}
	case LITEVM_SET_REGS: {
		struct litevm_regs litevm_regs;

		r = -EFAULT;
		if (copy_from_user(&litevm_regs, (void *)arg, sizeof litevm_regs))
			goto out;
		r = litevm_dev_ioctl_set_regs(litevm, &litevm_regs);
		if (r)
			goto out;
		r = 0;
		break;
	}
	case LITEVM_GET_SREGS: {
		struct litevm_sregs litevm_sregs;

		r = -EFAULT;
		if (copy_from_user(&litevm_sregs, (void *)arg, sizeof litevm_sregs))
			goto out;
		r = litevm_dev_ioctl_get_sregs(litevm, &litevm_sregs);
		if (r)
			goto out;
		r = -EFAULT;
		if (copy_to_user((void *)arg, &litevm_sregs, sizeof litevm_sregs))
			goto out;
		r = 0;
		break;
	}
	case LITEVM_SET_SREGS: {
		struct litevm_sregs litevm_sregs;

		r = -EFAULT;
		if (copy_from_user(&litevm_sregs, (void *)arg, sizeof litevm_sregs))
			goto out;
		r = litevm_dev_ioctl_set_sregs(litevm, &litevm_sregs);
		if (r)
			goto out;
		r = 0;
		break;
	}
	case LITEVM_TRANSLATE: {
		struct litevm_translation tr;

		r = -EFAULT;
		if (copy_from_user(&tr, (void *)arg, sizeof tr))
			goto out;
		r = litevm_dev_ioctl_translate(litevm, &tr);
		if (r)
			goto out;
		r = -EFAULT;
		if (copy_to_user((void *)arg, &tr, sizeof tr))
			goto out;
		r = 0;
		break;
	}
	case LITEVM_INTERRUPT: {
		struct litevm_interrupt irq;

		r = -EFAULT;
		if (copy_from_user(&irq, (void *)arg, sizeof irq))
			goto out;
		r = litevm_dev_ioctl_interrupt(litevm, &irq);
		if (r)
			goto out;
		r = 0;
		break;
	}
	case LITEVM_DEBUG_GUEST: {
		struct litevm_debug_guest dbg;

		r = -EFAULT;
		if (copy_from_user(&dbg, (void *)arg, sizeof dbg))
			goto out;
		r = litevm_dev_ioctl_debug_guest(litevm, &dbg);
		if (r)
			goto out;
		r = 0;
		break;
	}
	case LITEVM_SET_MEMORY_REGION: {
		struct litevm_memory_region litevm_mem;

		r = -EFAULT;
		if (copy_from_user(&litevm_mem, (void *)arg, sizeof litevm_mem))
			goto out;
		r = litevm_dev_ioctl_set_memory_region(litevm, &litevm_mem);
		if (r)
			goto out;
		break;
	}
	case LITEVM_GET_DIRTY_LOG: {
		struct litevm_dirty_log log;

		r = -EFAULT;
		if (copy_from_user(&log, (void *)arg, sizeof log))
			goto out;
		r = litevm_dev_ioctl_get_dirty_log(litevm, &log);
		if (r)
			goto out;
		break;
	}
	default:
		;
	}
out:
	return r;
}

static struct page *litevm_dev_nopage(struct vm_area_struct *vma,
				   unsigned long address,
				   int *type)
{
	struct litevm *litevm = vma->vm_file->private_data;
	unsigned long pgoff;
	struct litevm_memory_slot *slot;
	struct page *page;

	*type = VM_FAULT_MINOR;
	pgoff = ((address - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;
	slot = gfn_to_memslot(litevm, pgoff);
	if (!slot)
		return NOPAGE_SIGBUS;
	page = gfn_to_page(slot, pgoff);
	if (!page)
		return NOPAGE_SIGBUS;
	get_page(page);
	return page;
}

static struct vm_operations_struct litevm_dev_vm_ops = {
	.nopage = litevm_dev_nopage,
};

static int litevm_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	vma->vm_ops = &litevm_dev_vm_ops;
	return 0;
}

static struct file_operations litevm_chardev_ops = {
	.owner		= THIS_MODULE,
	.open		= litevm_dev_open,
	.release        = litevm_dev_release,
	.unlocked_ioctl = litevm_dev_ioctl,
	.compat_ioctl   = litevm_dev_ioctl,
	.mmap           = litevm_dev_mmap,
};

static struct miscdevice litevm_dev = {
	MISC_DYNAMIC_MINOR,
	"litevm",
	&litevm_chardev_ops,
};

static int litevm_reboot(struct notifier_block *notifier, unsigned long val,
                       void *v)
{
	if (val == SYS_RESTART) {
		/*
		 * Some (well, at least mine) BIOSes hang on reboot if
		 * in vmx root mode.
		 */
		printk(KERN_INFO "litevm: exiting vmx mode\n");
		on_each_cpu(litevm_disable, 0, 1);
	}
	return NOTIFY_OK;
}

static struct notifier_block litevm_reboot_notifier = {
	.notifier_call = litevm_reboot,
	.priority = 0,
};

static __init void litevm_init_debug(void)
{
	struct litevm_stats_debugfs_item *p;

	debugfs_dir = debugfs_create_dir("litevm", 0);
	for (p = debugfs_entries; p->name; ++p)
		p->dentry = debugfs_create_u32(p->name, 0444, debugfs_dir,
					       p->data);
}

static void litevm_exit_debug(void)
{
	struct litevm_stats_debugfs_item *p;

	for (p = debugfs_entries; p->name; ++p)
		debugfs_remove(p->dentry);
	debugfs_remove(debugfs_dir);
}

hpa_t bad_page_address;

static __init int litevm_init(void)
{
	static struct page *bad_page;
	int r = 0;

	if (!cpu_has_litevm_support()) {
		printk(KERN_ERR "litevm: no hardware support\n");
		return -EOPNOTSUPP;
	}
	if (vmx_disabled_by_bios()) {
		printk(KERN_ERR "litevm: disabled by bios\n");
		return -EOPNOTSUPP;
	}

	litevm_init_debug();

	setup_vmcs_descriptor();
	r = alloc_litevm_area();
	if (r)
		goto out;
	on_each_cpu(litevm_enable, 0, 1);
	register_reboot_notifier(&litevm_reboot_notifier);

	r = misc_register(&litevm_dev);
	if (r) {
		printk (KERN_ERR "litevm: misc device register failed\n");
		goto out_free;
	}


	if ((bad_page = alloc_page(GFP_KERNEL)) == NULL) {
		r = -ENOMEM;
		goto out_free;
	}

	bad_page_address = page_to_pfn(bad_page) << PAGE_SHIFT;
	memset(__va(bad_page_address), 0, PAGE_SIZE);

	return r;

out_free:
	free_litevm_area();
out:
	litevm_exit_debug();
	return r;
}

static __exit void litevm_exit(void)
{
	litevm_exit_debug();
	misc_deregister(&litevm_dev);
	unregister_reboot_notifier(&litevm_reboot_notifier);
	on_each_cpu(litevm_disable, 0, 1);
	free_litevm_area();
	__free_page(pfn_to_page(bad_page_address >> PAGE_SHIFT));
}

module_init(litevm_init)
module_exit(litevm_exit)
