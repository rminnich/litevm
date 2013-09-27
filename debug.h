#ifndef __LITEVM_DEBUG_H
#define __LITEVM_DEBUG_H

#ifdef LITEVM_DEBUG

void show_msrs(struct litevm_vcpu *vcpu);


void show_irq(struct litevm_vcpu *vcpu,  int irq);
void show_page(struct litevm_vcpu *vcpu, gva_t addr);
void show_u64(struct litevm_vcpu *vcpu, gva_t addr);
void show_code(struct litevm_vcpu *vcpu);
int vm_entry_test(struct litevm_vcpu *vcpu);

void vmcs_dump(struct litevm_vcpu *vcpu);
void regs_dump(struct litevm_vcpu *vcpu);
void sregs_dump(struct litevm_vcpu *vcpu);

#endif

#endif
