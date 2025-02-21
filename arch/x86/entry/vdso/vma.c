// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2007 Andi Kleen, SUSE Labs.
 *
 * This contains most of the x86 vDSO kernel-side code.
 */
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/random.h>
#include <linux/elf.h>
#include <linux/cpu.h>
#include <linux/ptrace.h>
#include <linux/time_namespace.h>

#include <asm/pvclock.h>
#include <asm/vgtod.h>
#include <asm/proto.h>
#include <asm/vdso.h>
#include <asm/vvar.h>
#include <asm/tlb.h>
#include <asm/page.h>
#include <asm/desc.h>
#include <asm/cpufeature.h>
#include <clocksource/hyperv_timer.h>

#undef _ASM_X86_VVAR_H
#define EMIT_VVAR(name, offset)	\
	const size_t name ## _offset = offset;
#include <asm/vvar.h>

struct vdso_data *arch_get_vdso_data(void *vvar_page)
{
	return (struct vdso_data *)(vvar_page + _vdso_data_offset);
}
#undef EMIT_VVAR

#if defined(CONFIG_X86_64)
unsigned int __read_mostly vdso64_enabled = 1;
#endif

void __init init_vdso_image(const struct vdso_image *image)
{
	BUG_ON(image->size % PAGE_SIZE != 0);

	apply_alternatives((struct alt_instr *)(image->data + image->alt),
			   (struct alt_instr *)(image->data + image->alt +
						image->alt_len));
}

static const struct vm_special_mapping vvar_mapping;
struct linux_binprm;

static vm_fault_t vdso_fault(const struct vm_special_mapping *sm,
		      struct vm_area_struct *vma, struct vm_fault *vmf)
{
	const struct vdso_image *image = vma->vm_mm->context.vdso_image;

	if (!image || (vmf->pgoff << PAGE_SHIFT) >= image->size)
		return VM_FAULT_SIGBUS;

	vmf->page = virt_to_page(image->data + (vmf->pgoff << PAGE_SHIFT));
	get_page(vmf->page);
	return 0;
}

static void vdso_fix_landing(const struct vdso_image *image,
		struct vm_area_struct *new_vma)
{
#if defined CONFIG_X86_32 || defined CONFIG_IA32_EMULATION
	if (in_ia32_syscall() && image == &vdso_image_32) {
		struct pt_regs *regs = current_pt_regs();
		unsigned long vdso_land = image->sym_int80_landing_pad;
		unsigned long old_land_addr = vdso_land +
			(unsigned long)current->mm->context.vdso;

		/* Fixing userspace landing - look at do_fast_syscall_32 */
		if (regs->ip == old_land_addr)
			regs->ip = new_vma->vm_start + vdso_land;
	}
#endif
}

static int vdso_mremap(const struct vm_special_mapping *sm,
		struct vm_area_struct *new_vma)
{
	unsigned long new_size = new_vma->vm_end - new_vma->vm_start;
	const struct vdso_image *image = current->mm->context.vdso_image;

	if (image->size != new_size)
		return -EINVAL;

	vdso_fix_landing(image, new_vma);
	current->mm->context.vdso = (void __user *)new_vma->vm_start;

	return 0;
}

static int vvar_mremap(const struct vm_special_mapping *sm,
		struct vm_area_struct *new_vma)
{
	const struct vdso_image *image = new_vma->vm_mm->context.vdso_image;
	unsigned long new_size = new_vma->vm_end - new_vma->vm_start;

	if (new_size != -image->sym_vvar_start)
		return -EINVAL;

	return 0;
}

#ifdef CONFIG_TIME_NS
static struct page *find_timens_vvar_page(struct vm_area_struct *vma)
{
	if (likely(vma->vm_mm == current->mm))
		return current->nsproxy->time_ns->vvar_page;

	/*
	 * VM_PFNMAP | VM_IO protect .fault() handler from being called
	 * through interfaces like /proc/$pid/mem or
	 * process_vm_{readv,writev}() as long as there's no .access()
	 * in special_mapping_vmops().
	 * For more details check_vma_flags() and __access_remote_vm()
	 */

	WARN(1, "vvar_page accessed remotely");

	return NULL;
}

/*
 * The vvar page layout depends on whether a task belongs to the root or
 * non-root time namespace. Whenever a task changes its namespace, the VVAR
 * page tables are cleared and then they will re-faulted with a
 * corresponding layout.
 * See also the comment near timens_setup_vdso_data() for details.
 */
int vdso_join_timens(struct task_struct *task, struct time_namespace *ns)
{
	struct mm_struct *mm = task->mm;
	struct vm_area_struct *vma;

	if (down_write_killable(&mm->mmap_sem))
		return -EINTR;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		unsigned long size = vma->vm_end - vma->vm_start;

		if (vma_is_special_mapping(vma, &vvar_mapping))
			zap_page_range(vma, vma->vm_start, size);
	}

	up_write(&mm->mmap_sem);
	return 0;
}
#else
static inline struct page *find_timens_vvar_page(struct vm_area_struct *vma)
{
	return NULL;
}
#endif

static vm_fault_t vvar_fault(const struct vm_special_mapping *sm,
		      struct vm_area_struct *vma, struct vm_fault *vmf)
{
	const struct vdso_image *image = vma->vm_mm->context.vdso_image;
	unsigned long pfn;
	long sym_offset;

	if (!image)
		return VM_FAULT_SIGBUS;

	sym_offset = (long)(vmf->pgoff << PAGE_SHIFT) +
		image->sym_vvar_start;

	/*
	 * Sanity check: a symbol offset of zero means that the page
	 * does not exist for this vdso image, not that the page is at
	 * offset zero relative to the text mapping.  This should be
	 * impossible here, because sym_offset should only be zero for
	 * the page past the end of the vvar mapping.
	 */
	if (sym_offset == 0)
		return VM_FAULT_SIGBUS;

	if (sym_offset == image->sym_vvar_page) {
		struct page *timens_page = find_timens_vvar_page(vma);

		pfn = __pa_symbol(&__vvar_page) >> PAGE_SHIFT;

		/*
		 * If a task belongs to a time namespace then a namespace
		 * specific VVAR is mapped with the sym_vvar_page offset and
		 * the real VVAR page is mapped with the sym_timens_page
		 * offset.
		 * See also the comment near timens_setup_vdso_data().
		 */
		if (timens_page) {
			unsigned long addr;
			vm_fault_t err;

			/*
			 * Optimization: inside time namespace pre-fault
			 * VVAR page too. As on timens page there are only
			 * offsets for clocks on VVAR, it'll be faulted
			 * shortly by VDSO code.
			 */
			addr = vmf->address + (image->sym_timens_page - sym_offset);
			err = vmf_insert_pfn(vma, addr, pfn);
			if (unlikely(err & VM_FAULT_ERROR))
				return err;

			pfn = page_to_pfn(timens_page);
		}

		return vmf_insert_pfn(vma, vmf->address, pfn);
	} else if (sym_offset == image->sym_pvclock_page) {
		struct pvclock_vsyscall_time_info *pvti =
			pvclock_get_pvti_cpu0_va();
		if (pvti && vclock_was_used(VCLOCK_PVCLOCK)) {
			return vmf_insert_pfn_prot(vma, vmf->address,
					__pa(pvti) >> PAGE_SHIFT,
					pgprot_decrypted(vma->vm_page_prot));
		}
	} else if (sym_offset == image->sym_hvclock_page) {
		struct ms_hyperv_tsc_page *tsc_pg = hv_get_tsc_page();

		if (tsc_pg && vclock_was_used(VCLOCK_HVCLOCK))
			return vmf_insert_pfn(vma, vmf->address,
					virt_to_phys(tsc_pg) >> PAGE_SHIFT);
	} else if (sym_offset == image->sym_timens_page) {
		struct page *timens_page = find_timens_vvar_page(vma);

		if (!timens_page)
			return VM_FAULT_SIGBUS;

		pfn = __pa_symbol(&__vvar_page) >> PAGE_SHIFT;
		return vmf_insert_pfn(vma, vmf->address, pfn);
	}

	return VM_FAULT_SIGBUS;
}

static const struct vm_special_mapping vdso_mapping = {
	.name = "[vdso]",
	.fault = vdso_fault,
	.mremap = vdso_mremap,
};
static const struct vm_special_mapping vvar_mapping = {
	.name = "[vvar]",
	.fault = vvar_fault,
	.mremap = vvar_mremap,
};

/*
 * Add vdso and vvar mappings to current process.
 * @image          - blob to map
 * @addr           - request a specific address (zero to map at free addr)
 */
static int map_vdso(const struct vdso_image *image, unsigned long addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long text_start;
	int ret = 0;

	if (down_write_killable(&mm->mmap_sem))
		return -EINTR;

	addr = get_unmapped_area(NULL, addr,
				 image->size - image->sym_vvar_start, 0, 0);
	if (IS_ERR_VALUE(addr)) {
		ret = addr;
		goto up_fail;
	}

	text_start = addr - image->sym_vvar_start;

	/*
	 * MAYWRITE to allow gdb to COW and set breakpoints
	 */
	vma = _install_special_mapping(mm,
				       text_start,
				       image->size,
				       VM_READ|VM_EXEC|
				       VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC,
				       &vdso_mapping);

	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto up_fail;
	}

	vma = _install_special_mapping(mm,
				       addr,
				       -image->sym_vvar_start,
				       VM_READ|VM_MAYREAD|VM_IO|VM_DONTDUMP|
				       VM_PFNMAP,
				       &vvar_mapping);

	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		do_munmap(mm, text_start, image->size, NULL);
	} else {
		current->mm->context.vdso = (void __user *)text_start;
		current->mm->context.vdso_image = image;
	}

up_fail:
	up_write(&mm->mmap_sem);
	return ret;
}

#ifdef CONFIG_X86_64
static int map_vdso_randomized(const struct vdso_image *image)
{
	return map_vdso(image, 0);
}
#endif

int map_vdso_once(const struct vdso_image *image, unsigned long addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;

	down_write(&mm->mmap_sem);
	/*
	 * Check if we have already mapped vdso blob - fail to prevent
	 * abusing from userspace install_speciall_mapping, which may
	 * not do accounting and rlimit right.
	 * We could search vma near context.vdso, but it's a slowpath,
	 * so let's explicitly check all VMAs to be completely sure.
	 */
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma_is_special_mapping(vma, &vdso_mapping) ||
				vma_is_special_mapping(vma, &vvar_mapping)) {
			up_write(&mm->mmap_sem);
			return -EEXIST;
		}
	}
	up_write(&mm->mmap_sem);

	return map_vdso(image, addr);
}

#if defined(CONFIG_X86_32) || defined(CONFIG_IA32_EMULATION)
static int load_vdso32(void)
{
	if (vdso32_enabled != 1)  /* Other values all mean "disabled" */
		return 0;

	return map_vdso(&vdso_image_32, 0);
}
#endif

#ifdef CONFIG_X86_64
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	if (!vdso64_enabled)
		return 0;

	return map_vdso_randomized(&vdso_image_64);
}

#ifdef CONFIG_COMPAT
int compat_arch_setup_additional_pages(struct linux_binprm *bprm,
				       int uses_interp)
{
#ifdef CONFIG_X86_X32_ABI
	if (test_thread_flag(TIF_X32)) {
		if (!vdso64_enabled)
			return 0;
		return map_vdso_randomized(&vdso_image_x32);
	}
#endif
#ifdef CONFIG_IA32_EMULATION
	return load_vdso32();
#else
	return 0;
#endif
}
#endif
#else
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	return load_vdso32();
}
#endif

#ifdef CONFIG_X86_64
static __init int vdso_setup(char *s)
{
	vdso64_enabled = simple_strtoul(s, NULL, 0);
	return 0;
}
__setup("vdso=", vdso_setup);

static int __init init_vdso(void)
{
	init_vdso_image(&vdso_image_64);

#ifdef CONFIG_X86_X32_ABI
	init_vdso_image(&vdso_image_x32);
#endif

	return 0;
}
subsys_initcall(init_vdso);
#endif /* CONFIG_X86_64 */
