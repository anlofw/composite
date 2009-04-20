/**
 * Copyright 2007 by Gabriel Parmer, gabep1@cs.bu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#include "include/ipc.h"
#include "include/spd.h"
#include "include/debug.h"
#include "include/measurement.h"
#include "include/mmap.h"

#include <linux/kernel.h>

/* 
 * These are the 1) page for the pte for the shared region and 2) the
 * page to hold general data including cpuid, thread id, identity
 * info, etc...  These are placed here so that we don't have to go
 * through a variable to find their address, so that lookup and
 * manipulation are quick.
 */
unsigned int shared_region_page[1024] PAGE_ALIGNED;
unsigned int shared_data_page[1024] PAGE_ALIGNED;

static inline struct shared_user_data *get_shared_data(void)
{
	return (struct shared_user_data*)shared_data_page;
}

#define COS_SYSCALL __attribute__((regparm(0)))

/* 
 * This variable tracks the number of cycles that have elapsed since
 * the last measurement and is typically used to measure how long
 * brand threads execute.
 */
static unsigned long cycle_cnt;

void ipc_init(void)
{
	//memset(shared_region_page, 0, PAGE_SIZE);
	memset(shared_data_page, 0, PAGE_SIZE);
	rdtscl(cycle_cnt);

	return;
}

static inline void open_spd(struct spd_poly *spd)
{
	printk("cos: open_spd (asymmetric trust) not supported on x86.\n");
	
	return;
}

extern void switch_host_pg_tbls(phys_addr_t pt);
static inline void switch_pg_tbls(phys_addr_t new, phys_addr_t old)
{
	if (likely(old != new)) {
		native_write_cr3(new);
		switch_host_pg_tbls(new);
	}

	return;
}

static inline void open_close_spd(struct spd_poly *o_spd, struct spd_poly *c_spd)
{
	switch_pg_tbls(o_spd->pg_tbl, c_spd->pg_tbl);

	return;
}

static inline void open_close_spd_ret(struct spd_poly *c_spd) /*, struct spd_poly *s_spd)*/
{
	native_write_cr3(c_spd->pg_tbl);
	switch_host_pg_tbls(c_spd->pg_tbl);
	
	return;
}

static void print_stack(struct thread *thd)
{
	int i;

	printk("cos: In thd %x, stack:\n", thd_get_id(thd));
	for (i = 0 ; i <= thd->stack_ptr ; i++) {
		struct thd_invocation_frame *frame = &thd->stack_base[i];
		printk("cos: \t[spd %d]\n", spd_get_index(frame->spd));
	}
}

void print_regs(struct pt_regs *regs)
{
	printk("cos: EAX:%x\tEBX:%x\tECX:%x\n"
	       "cos: EDX:%x\tESI:%x\tEDI:%x\n"
	       "cos: EIP:%x\tESP:%x\tEBP:%x\n",
	       (unsigned int)regs->eax, (unsigned int)regs->ebx, (unsigned int)regs->ecx,
	       (unsigned int)regs->edx, (unsigned int)regs->esi, (unsigned int)regs->edi,
	       (unsigned int)regs->eip, (unsigned int)regs->esp, (unsigned int)regs->ebp);

	return;
}

struct inv_ret_struct {
	int thd_id;
	int spd_id;
};

extern struct invocation_cap invocation_capabilities[MAX_STATIC_CAP];
/* 
 * FIXME: 1) should probably return the static capability to allow
 * isolation level isolation access from caller, 2) all return 0
 * should kill thread.
 */
COS_SYSCALL vaddr_t ipc_walk_static_cap(struct thread *thd, unsigned int capability, 
					vaddr_t sp, vaddr_t ip, /*vaddr_t usr_def, */
					struct inv_ret_struct *ret)
{
	struct thd_invocation_frame *curr_frame;
	struct spd *curr_spd, *dest_spd;
	struct invocation_cap *cap_entry;

	capability >>= 20;

	if (unlikely(capability >= MAX_STATIC_CAP)) {
		struct spd *t = virtual_namespace_query(ip);
		printk("cos: capability %d greater than max from spd %d @ %x.\n", 
		       capability, (t) ? spd_get_index(t): 0, (unsigned int)ip);
		return 0;
	}

	cap_entry = &invocation_capabilities[capability];

	if (unlikely(!cap_entry->owner)) {
		printk("cos: No owner for cap %d.\n", capability);
		return 0;
	}

	/* what spd are we in (what stack frame)? */
	curr_frame = &thd->stack_base[thd->stack_ptr];

	dest_spd = cap_entry->destination;
	curr_spd = cap_entry->owner;

	if (unlikely(!dest_spd || curr_spd == CAP_FREE || curr_spd == CAP_ALLOCATED_UNUSED)) {
		printk("cos: Attempted use of unallocated capability.\n");
		return 0;
	}

	/*
	 * If the spd that owns this capability is part of a composite
	 * spd that is the same as the composite spd that was the
	 * entry point for this composite spd.
	 *
	 * i.e. is the capability owner in the same protection domain
	 * (via ST) as the spd entry point to the protection domain.
	 *
	 * We are doing a repetitive calculation for the first check
	 * here and in the thd_spd_in_current_composite, as we want to
	 * avoid making the function call here if possible.  FIXME:
	 * should just use a specific inlined method here to avoid
	 * this.
	 */
	if (unlikely(!thd_spd_in_composite(curr_frame->current_composite_spd, curr_spd))) {
		printk("cos: Error, incorrect capability (Cap %d has cspd %x, stk has %x).\n",
		       capability, spd_get_index(curr_spd), spd_get_index(curr_frame->spd));
		print_stack(thd);
		/* 
		 * FIXME: do something here like throw a fault to be
		 * handled by a user-level handler
		 */
		return 0;
	}

	cap_entry->invocation_cnt++;

	/***************************************************************
	 * IMPORTANT FIXME: Not only do we want to switch the page     *
	 * tables here, but if there is any chance that we will block, *
	 * then we should change current->mm->pgd =                    *
	 * pa_to_va(dest_spd->composite_spd->pg_tbl).  In practice     *
	 * there it is almost certainly probably that we _can_ block,  *
	 * so we probably need to do this.                             *
	 ***************************************************************/

//	if (cap_entry->il & IL_INV_UNMAP) {
	open_close_spd(dest_spd->composite_spd, curr_spd->composite_spd);
//	} else {
//		open_spd(&curr_spd->spd_info);
//	}

	ret->thd_id = thd->thread_id;
	ret->spd_id = spd_get_index(curr_spd);

	spd_mpd_ipc_take((struct composite_spd *)dest_spd->composite_spd);

	/* 
	 * ref count the composite spds:
	 * 
	 * FIXME, TODO: move composite pgd into each spd and ref count
	 * in spds.  Sum of all ref cnts is the composite ref count.
	 * This will eliminate the composite cache miss.
	 */
	
	/* add a new stack frame for the spd we are invoking (we're committed) */
	thd_invocation_push(thd, cap_entry->destination, sp, ip);

	cos_meas_event(COS_MEAS_INVOCATIONS);

	return cap_entry->dest_entry_instruction;
}

static struct pt_regs *brand_execution_completion(struct thread *);
static struct pt_regs *thd_ret_term_upcall(struct thread *t);
/*
 * Return from an invocation by popping off of the invocation stack an
 * entry, and returning its contents (including return ip and sp).
 * This is complicated by the fact that we may return when no
 * invocation is made because a thread is terminating.
 */
COS_SYSCALL struct thd_invocation_frame *pop(struct thread *curr, struct pt_regs **regs_restore)
{
	struct thd_invocation_frame *inv_frame;
	struct thd_invocation_frame *curr_frame;

	inv_frame = thd_invocation_pop(curr);

	/* At the top of the invocation stack? */
	if (unlikely(inv_frame == NULL)) {
		assert(!(curr->flags & THD_STATE_READY_UPCALL));
		if (curr->flags & THD_STATE_ACTIVE_UPCALL) {
			/* If we are an upcall, then complete the
			 * upcall, and either execute a pending,
			 * switch to a preempted thread, or upcall to
			 * the scheduler */
			*regs_restore = brand_execution_completion(curr);
		} else {
			/* normal thread terminates: upcall into root
			 * scheduler */
			*regs_restore = thd_ret_term_upcall(curr);
		}

		return NULL;
	}
	
	curr_frame = thd_invstk_top(curr);
	/* for now just assume we always close the server spd */
	open_close_spd_ret(curr_frame->current_composite_spd);

	/*
	 * FIXME: If an invocation causes a "needless" kernel
	 * invocation/pop even when the two spds are in the same
	 * composite spd (but weren't at some point in the future),
	 * then we really probably shouldn't release here.
	 *
	 * This REALLY should be spd_mpd_release.
	 */
	//cos_ref_release(&inv_frame->current_composite_spd->ref_cnt);
	//spd_mpd_release((struct composite_spd *)inv_frame->current_composite_spd);
	spd_mpd_ipc_release((struct composite_spd *)inv_frame->current_composite_spd);

	return inv_frame;	
}

/********** Composite system calls **********/

COS_SYSCALL int cos_syscall_void(int spd_id)
{
	printk("cos: error - made void system call\n");

	return 0;
}

extern int switch_thread_data_page(int old_thd, int new_thd);
struct thread *ready_boot_thread(struct spd *init)
{
	struct shared_user_data *ud = get_shared_data();
	struct thread *thd;
	unsigned int tid;

	assert(NULL != init);

	thd = thd_alloc(init);
	if (NULL == thd) {
		printk("cos: Could not allocate boot thread.\n");
		return NULL;
	}
	assert(thd_get_id(thd) == 1);
	tid = thd_get_id(thd);
	thd_set_current(thd);

	switch_thread_data_page(2, tid);
	/* thread ids start @ 1 */
	ud->current_thread = tid;
	ud->argument_region = (void*)((tid * PAGE_SIZE) + COS_INFO_REGION_ADDR);

	return thd;
}

void switch_thread_context(struct thread *curr, struct thread *next)
{
	struct shared_user_data *ud = get_shared_data();
	unsigned int ctid, ntid;
	struct spd_poly *cspd, *nspd;

	assert(thd_get_current() != next);

	ctid = thd_get_id(curr);
	ntid = thd_get_id(next);
	thd_set_current(next);

	switch_thread_data_page(ctid, ntid);
	/* thread ids start @ 1, thus thd pages are offset above the data page */
	ud->current_thread = ntid;
	ud->argument_region = (void*)((ntid * PAGE_SIZE) + COS_INFO_REGION_ADDR);

	cspd = thd_get_thd_spdpoly(curr);
	nspd = thd_get_thd_spdpoly(next);
	
	open_close_spd(nspd, cspd);

	return;
}

void initialize_sched_info(struct thread *t, struct spd *curr_sched)
{
	struct spd *sched;
	int i;

	assert(spd_is_scheduler(curr_sched));
	/* 
	 * Initialize the thread's path through its hierarchy of
	 * schedulers.  They will have to explicitly set the
	 * thread_notification location at a later time.
	 *
	 * OPTION: Another option here would be to simply copy the
	 * scheduler hierarchy of the current thread.  A good way to
	 * initialize the urgency values for all the schedulers.
	 */
	sched = curr_sched;
	for (i = curr_sched->sched_depth ; i >= 0 ; i--) {
		struct thd_sched_info *tsi = thd_get_sched_info(t, i);

		tsi->scheduler = sched;
		tsi->thread_notifications = NULL;

		sched = sched->parent_sched;
	}

	return;
}

/*
 * Note here that we still copy the empty structures for simplicity.
 */
static inline void copy_sched_info_structs(struct thd_sched_info *new, 
					   struct thd_sched_info *old, int num)
{
	int i;
	struct spd *prev_sched = NULL;

	assert(num < MAX_SCHED_HIER_DEPTH);

	for (i = 0 ; i < num ; i++) {
		if (old[i].scheduler) {
			assert(old[i].scheduler->parent_sched == prev_sched);
		}

		prev_sched = old[i].scheduler;
		new[i].scheduler = prev_sched;
		new[i].thread_notifications = old[i].thread_notifications;
		new[i].notification_offset = old[i].notification_offset;
	}

	return;
}

void copy_sched_info(struct thread *new, struct thread *old)
{
	copy_sched_info_structs(new->sched_info, old->sched_info, MAX_SCHED_HIER_DEPTH-1);
}

/* 
 * Hope the current thread saved its context...should be able to
 * resume_return to it.
 *
 * TODO: should NOT pass in fn and stack here.  Should simply upcall
 * into cos_upcall_entry, and pass in the data.  The data should point
 * to the fn and stack to be used anyway, which can be assigned at
 * user-level.
 */
COS_SYSCALL int cos_syscall_create_thread(int spd_id, int a, int b, int c)
{
	struct thread *thd, *curr;
	struct spd *curr_spd;

	/*
	 * Lets make sure that the current spd is a scheduler and has
	 * scheduler control of the current thread before we let it
	 * create any threads.
	 *
	 * FIXME: in the future, I should really just allow the base
	 * scheduler to create threads, i.e. when 0 == sched_depth.
	 */
	curr = thd_get_current();
	curr_spd = thd_validate_get_current_spd(curr, spd_id);
	if (NULL == curr_spd) {
		printk("cos: component claimed in spd %d, but not\n", spd_id);
		return -1;
	}

	if (!spd_is_scheduler(curr_spd) || !thd_scheduled_by(curr, curr_spd)) {
		printk("cos: non-scheduler attempted to create thread.\n");
		return -1;
	}

	thd = thd_alloc(curr_spd);
	if (thd == NULL) {
		printk("cos: Could not allocate thread\n");
		return -1;
	}

	/* FIXME: switch to using upcall_setup here */
	thd->stack_ptr = 0;
	thd->stack_base[0].current_composite_spd = curr_spd->composite_spd;
	thd->stack_base[0].spd = curr_spd;
	/* FIXME: do this lazily */
	spd_mpd_ipc_take((struct composite_spd *)curr_spd->composite_spd);

	thd->regs.ecx = COS_UPCALL_CREATE;
	thd->regs.edx = curr_spd->upcall_entry;
	thd->regs.ebx = a;
	thd->regs.edi = b;	
	thd->regs.esi = c;
	thd->regs.eax = thd_get_id(thd);

	thd->flags |= THD_STATE_CYC_CNT;
	initialize_sched_info(thd, curr_spd);
	
	return thd_get_id(thd);
}

COS_SYSCALL int cos_syscall_thd_cntl(int spd_id, int op_thdid, long arg1, long arg2)
{
	struct thread *thd, *curr;
	struct spd *curr_spd;
	short int op, thdid;

	op = op_thdid >> 16;
	thdid = op_thdid & 0xFFFF;
	/*
	 * Lets make sure that the current spd is a scheduler and has
	 * scheduler control of the current thread before we let it
	 * create any threads.
	 *
	 * FIXME: in the future, I should really just allow the base
	 * scheduler to create threads, i.e. when 0 == sched_depth.
	 */
	curr = thd_get_current();
	curr_spd = thd_validate_get_current_spd(curr, spd_id);
	if (NULL == curr_spd) {
		printk("cos: component claimed in spd %d, but not\n", spd_id);
		return -1;
	}
	
	thd = thd_get_by_id(thdid);
	if (!spd_is_scheduler(curr_spd) || !thd_scheduled_by(thd, curr_spd)) {
		printk("cos: non-scheduler attempted to create thread.\n");
		return -1;
	}
	
	switch (op) {
	case COS_THD_INV_FRAME:
	{
		struct spd *i_spd;
		int frame_offset = arg1;
		struct thd_invocation_frame *tif;

		/* Offset out of bounds */
		if (frame_offset > thd->stack_ptr) return 0;
		tif = &thd->stack_base[frame_offset];
		i_spd = tif->spd;
		return spd_get_index(i_spd);
	}
	case COS_THD_INVFRM_IP:
	{
		int frame_offset = arg1;

		/* FIXME: broken for "current thread" */
		/* Offset out of bounds */
		if (frame_offset > thd->stack_ptr) return 0;
		if (frame_offset == thd->stack_ptr) {
			if (thd->flags & THD_STATE_PREEMPTED) {
				return thd->regs.eip;
			} else {
				return thd->regs.edx;
			}
		} else {
			struct thd_invocation_frame *tif;
			tif = &thd->stack_base[frame_offset+1];
			return tif->ip;
		}
	}
	default:
		printk("cos: undefined operation %d for thread %d from scheduler %d.\n",
		       op, thdid, spd_id);
		return -1;
	}
	return 0;
}

static inline void remove_preempted_status(struct thread *thd)
{
 	if (thd->preempter_thread) {
		struct thread *p = thd->preempter_thread;
		struct thread *i = thd->interrupted_thread;

		/* is the doubly linked list sound? */
		assert(p->interrupted_thread == thd);
			
		/* break the doubly linked list of interrupted thds */
		p->interrupted_thread = NULL;
		if (i) {
			i->preempter_thread = NULL;
		}
		thd->preempter_thread = NULL;
		thd->interrupted_thread = NULL;
	}

	thd->flags &= ~THD_STATE_PREEMPTED;
}

extern int cos_syscall_switch_thread(void);
void update_sched_evts(struct thread *new, int new_flags, 
		       struct thread *prev, int prev_flags);
static struct pt_regs *sched_tailcall_pending_upcall(struct thread *uc, 
						     struct composite_spd *curr);
static inline void update_thd_evt_state(struct thread *t, int flags, int update_list);
static inline void break_preemption_chain(struct thread *t)
{
	struct thread *other;

	other = t->interrupted_thread;
	if (other) {
		assert(other->preempter_thread == t);
		t->interrupted_thread = NULL;
		other->preempter_thread = NULL;
	}
	other = t->preempter_thread;
	if (other) {
		assert(other->interrupted_thread == t);
		t->preempter_thread = NULL;
		other->interrupted_thread = NULL;
	}
}

/*
 * The arguments are horrible as we are interfacing w/ assembly and 1)
 * we need to return two values, the regs to restore, and if the next
 * thread was preempted or not (totally different return sequences),
 * 2) all syscalls provide as the first argument the spd_id of the spd
 * making the syscalls.  We need this info, and it is on the stack,
 * but (1) interferes with that as it pushes values onto the stack.  A
 * more pleasant way to deal with this might be to pass the args in
 * registers.  see ipc.S cos_syscall_switch_thread.
 */
COS_SYSCALL struct pt_regs *cos_syscall_switch_thread_cont(int spd_id, unsigned short int rthd_id, 
							   unsigned short int rflags, long *preempt)
{
	struct thread *thd, *curr;
	struct spd *curr_spd;
	unsigned short int next_thd, flags, curr_sched_flags = COS_SCHED_EVT_NIL;
	struct cos_sched_data_area *da;

	*preempt = 0;
	curr = thd_get_current();
	curr_spd = thd_validate_get_current_spd(curr, spd_id);
	if (NULL == curr_spd) {
		printk("cos: component claimed in spd %d, but not\n", spd_id);
		curr->regs.eax = -1;
		return &curr->regs;
	}

	assert(!(curr->flags & THD_STATE_PREEMPTED));

	da = curr_spd->sched_shared_page;
	if (unlikely(NULL == da)) {
		printk("cos: non-scheduler attempting to switch thread.\n");
		curr->regs.eax = -1;
		return &curr->regs;
	}

	if (rflags & (COS_SCHED_SYNC_BLOCK | COS_SCHED_SYNC_UNBLOCK)) {
		next_thd = rthd_id;
		/* FIXME: mask out all flags that can't apply here  */
	} else {
		next_thd = da->cos_next.next_thd_id;
		da->cos_next.next_thd_id = 0;
		/* FIXME: mask out the locking flags as they cannot apply */
	}
	/* 
	 * So far all flags should be taken in the context of the
	 * actual invoking thread (they effect the thread switching
	 * _from_ rather than the thread to switch _to_ in which case
	 * we would want to use the sched_page flags.
	 */
	flags = rflags;

	/* 
	 * Uncommon, but valid case: between when the current thread
	 * executed through the scheduler and when the switch_thread
	 * system call was made, that thread was preempted, or an
	 * event occurred (which zeros out next_thd).  This simply
	 * means that the current system call doesn't have all the
	 * information (which upcalls are active, if threads have been
	 * woken up, etc...), so we should make it go through the
	 * scheduling process again.
	 */
	if (0 == next_thd) {
		cos_meas_event(COS_MEAS_SWITCH_OUTDATED);
		curr->regs.eax = 1;
		return &curr->regs;
	}
	thd = thd_get_by_id(next_thd);


#ifdef NIL
	if (2 == next_thd) {
		struct thread *t;

		t = thd_get_by_id(15);
		if (t) {
			if (t->flags & THD_STATE_ACTIVE_UPCALL) {
				struct thd_sched_info *tsi;
				unsigned long ts;
				tsi = thd_get_sched_info(t, 0);
				assert(tsi);
				rdtscl(ts);
				printk("cos: idle w/uc @ %ld, (from %d) evt, flgs %x, nxt %d\n", 
				       ts, thd_get_id(curr),
				       COS_SCHED_EVT_FLAGS(tsi->thread_notifications),
				       COS_SCHED_EVT_NEXT(tsi->thread_notifications));
			}
		}
	}
#endif

	/* error cases */
	if (unlikely(thd == curr)) {
		cos_meas_event(COS_MEAS_SWITCH_SELF);
		curr->regs.eax = 1;
		return &curr->regs;
	}
	if (unlikely(NULL == thd)) {
		printk("cos: no thread with id %d and flags %x, cannot switch to it from %d @ %x.\n", 
		       next_thd, flags, thd_get_id(curr), (unsigned int)curr->regs.edx);
		thd_print_regs(curr);

		curr->regs.eax = -1;
		return &curr->regs;
	}
	if (unlikely(!thd_scheduled_by(curr, curr_spd) ||
		     !thd_scheduled_by(thd, curr_spd))) {
		printk("cos: scheduler %d does not have scheduling control over %d or %d, cannot switch.\n",
		       spd_get_index(curr_spd), thd_get_id(curr), thd_get_id(thd));
		curr->regs.eax = -1;
		return &curr->regs;
	}
	/* we cannot schedule to run an upcall thread that is not running */
	if (unlikely(thd->flags & THD_STATE_READY_UPCALL)) {
		static int first = 1;
		if (first) {
			printk("cos: upcall thd %d not ready to run (current %d) -- this message will not repeat.\n", thd_get_id(thd), thd_get_id(curr));
			first = 0;
		}
		cos_meas_event(COS_MEAS_UPCALL_INACTIVE);
		curr->regs.eax = -2;
		return &curr->regs;
	}

	/* If a thread is involved in a scheduling decision, we should
	 * assume that any preemption chains that existed aren't valid
	 * anymore. */
	break_preemption_chain(curr);

	if (thd->flags & THD_STATE_ACTIVE_UPCALL) {
		cos_meas_stats_end(COS_MEAS_STATS_UC_EXEC_DELAY, 1);
	}

//	printk("%d->%d\n", thd_get_id(curr), thd_get_id(thd));

	if (flags & COS_SCHED_TAILCALL) {
		if (!(curr->flags & THD_STATE_ACTIVE_UPCALL &&
		      curr->stack_ptr == 0)) {
			printk("cos: illegal use of tailcall.");
			curr->regs.eax = -1;
			return &curr->regs;
		}
		assert(!(curr->flags & THD_STATE_READY_UPCALL));

		cos_meas_stats_end(COS_MEAS_STATS_UC_TERM_DELAY, 1);
		cos_meas_stats_end(COS_MEAS_STATS_UC_PEND_DELAY, 0);

		spd_mpd_ipc_release((struct composite_spd *)thd_get_thd_spdpoly(curr));
		assert(curr->thread_brand);
		if (curr->thread_brand->pending_upcall_requests) {
			//update_thd_evt_state(curr, COS_SCHED_EVT_BRAND_ACTIVE, 1);
			//spd_mpd_ipc_release((struct composite_spd *)thd_get_thd_spdpoly(curr));
			cos_meas_event(COS_MEAS_BRAND_COMPLETION_PENDING);
			return sched_tailcall_pending_upcall(curr, (struct composite_spd*)curr_spd->composite_spd);
		} else {
			/* Can't really be tailcalling and the other
			 * flags at the same time */
			if (flags & (THD_STATE_SCHED_EXCL | COS_SCHED_SYNC_BLOCK | COS_SCHED_SYNC_UNBLOCK)) {
				printk("cos: cannot switch using tailcall and other options %d\n", flags);
				curr->regs.eax = -1;
				return &curr->regs;
			}
 			
			curr->flags &= ~THD_STATE_ACTIVE_UPCALL;
			curr->flags |= THD_STATE_READY_UPCALL;
			curr->sched_suspended = NULL;
			//spd_mpd_ipc_release((struct composite_spd *)thd_get_thd_spdpoly(curr));
			/***********************************************
			 * FIXME: call pt_regs *brand_execution_completion(struct thread *curr)?
			 ***********************************************/
			
			cos_meas_event(COS_MEAS_FINISHED_BRANDS);
			cos_meas_event(COS_MEAS_BRAND_COMPLETION_TAILCALL);
			/*
			 * For general support:
			 *
			 * Check that the spd from the invocation frame popped
			 * off of the thread's stack matches curr_spd (or else
			 * we were called via ST from another spd and should
			 * return via normal control flow to it.
			 *
			 * If that's fine, then execute code similar to pop
			 * above (to return from an invocation).
			 */
			curr_sched_flags = COS_SCHED_EVT_BRAND_READY;
		}
	}

	/*
	 * If the thread was suspended by another scheduler, we really
	 * have no business resuming it IF that scheduler wants
	 * exclusivity for scheduling and we are not the parent of
	 * that scheduler.
	 */
	if (thd->flags & THD_STATE_SCHED_EXCL) {
		struct spd *suspender = thd->sched_suspended;

		if (suspender && curr_spd->sched_depth > suspender->sched_depth) {
			printk("cos: scheduler %d resuming thread %d, but spd %d suspended it.\n",
			       spd_get_index(curr_spd), thd_get_id(thd), spd_get_index(thd->sched_suspended));
			curr->regs.eax = -1;
			return &curr->regs;
		}
		thd->flags &= ~THD_STATE_SCHED_EXCL;
	}
	if (flags & COS_SCHED_EXCL_YIELD) {
		curr->flags |= THD_STATE_SCHED_EXCL;
	}

	/*** A synchronization event for the scheduler? ***/
	if (flags & COS_SCHED_SYNC_BLOCK) {
		struct cos_synchronization_atom *l = &da->cos_locks;
		
		/* if a thread's version of which thread should be
		 * scheduled next does not comply with the in-memory
		 * version within the lock, then we are dealing with a
		 * stale invocation.
		 */
		if (l->owner_thd != next_thd) {
			cos_meas_event(COS_MEAS_ATOMIC_STALE_LOCK);
			curr->regs.eax = 0;
			return &curr->regs;
		}
		cos_meas_event(COS_MEAS_ATOMIC_LOCK);

		/* FIXME: this should only be set if it is the most
		 * urgent of the blocked threads waiting for owner_thd
		 * to complete.
		 */
		l->queued_thd = thd_get_id(curr);
		/* 
		 * FIXME: alter the urgency/priority of the owner
		 * thread to inherit that of the current blocked thd.
		 */
	} else if (flags & COS_SCHED_SYNC_UNBLOCK) {
		cos_meas_event(COS_MEAS_ATOMIC_UNLOCK);
		/* 
		 * FIXME: reset urgency/priority of current thread back
		 * to natural state.
		 */
	}

	curr->sched_suspended = curr_spd;
	thd->sched_suspended = NULL;

	switch_thread_context(curr, thd);

	if (thd->flags & THD_STATE_PREEMPTED) {
		cos_meas_event(COS_MEAS_SWITCH_PREEMPT);
		remove_preempted_status(thd);
		*preempt = 1;
	} else {
		cos_meas_event(COS_MEAS_SWITCH_COOP);
	}

	/* If we are an upcalling thread, and we are asking to return,
	 * we're done.  If a preemption thread, deactivate.  */
	if (flags & COS_SCHED_TAILCALL && 
	    curr->stack_ptr == 0 && 
	    curr->flags & THD_STATE_ACTIVE_UPCALL) {
//		assert(!curr->preempter_thread); 
//		assert(!curr->interrupted_thread);
	}

	/*
	if (!(flags & COS_SCHED_TAILCALL) && 
	    curr->flags & THD_STATE_ACTIVE_UPCALL) {
		printk("cos: Error, error, upcall thread not tailcalling\n");
	}
	*/

	update_sched_evts(thd, COS_SCHED_EVT_NIL, curr, curr_sched_flags);
	/* success for this current thread */
	curr->regs.eax = 0;
	
	return &thd->regs;
}

extern void cos_syscall_kill_thd(int thd_id);
COS_SYSCALL void cos_syscall_kill_thd_cont(int spd_id, int thd_id)
{
	printk("cos: killing threads not yet supported.\n");

	return;

}

static struct thread *upcall_setup(struct thread *uc, struct spd *dest, upcall_type_t option,
				   long arg1, long arg2, long arg3)
{
	struct pt_regs *r = &uc->regs;

	r->ebx = arg1;
	r->edi = arg2;
	r->esi = arg3;
	r->ecx = option;
	r->eip = r->edx = dest->upcall_entry;
	r->eax = thd_get_id(uc);

	uc->stack_ptr = 0;
	uc->stack_base[0].current_composite_spd = dest->composite_spd;
	uc->stack_base[0].spd = dest;
	spd_mpd_ipc_take((struct composite_spd *)dest->composite_spd);

	return uc;
}

static struct thread *upcall_execute(struct thread *uc, struct thread *prev, 
				     struct composite_spd *old)
{
	struct composite_spd *cspd = (struct composite_spd*)uc->stack_base[0].current_composite_spd;

	if (prev && prev != uc) {
		/* This will switch the page tables for us */
		switch_thread_context(prev, uc);
	} else if (old != cspd) {
		/* we have already released the old->composite_spd */
		open_close_spd(&cspd->spd_info, &old->spd_info);
	}

	return uc;
}

/* Upcall into base scheduler! */
static struct pt_regs *thd_ret_term_upcall(struct thread *curr)
{
	struct composite_spd *cspd = (struct composite_spd *)thd_get_thd_spdpoly(curr);
	struct thd_sched_info *tsi;
	struct spd *dest;
	assert(cspd);
	spd_mpd_ipc_release(cspd);
	
	tsi = thd_get_sched_info(curr, 0);
	dest = tsi->scheduler;
	
	upcall_setup(curr, dest, COS_UPCALL_DESTROY, 0, 0, 0);
	upcall_execute(curr, NULL, cspd);
	
	return &curr->regs;
}

//static int cos_net_try_packet(struct thread *brand, unsigned short int *port);

/* 
 * Assumes: we are called from the thread switching syscall, with the
 * TAIL_CALL flag (i.e. we are switching away from an upcall).  Also,
 * that the previous component was released.
 */
static struct pt_regs *sched_tailcall_pending_upcall(struct thread *uc, struct composite_spd *curr)
{
	struct thread *brand = uc->thread_brand;
	struct spd *dest;
//	unsigned short int port;
	//struct composite_spd *curr;

	assert(brand && brand->pending_upcall_requests > 0);
	assert(uc->flags & THD_STATE_ACTIVE_UPCALL && 
	       !(uc->flags & THD_STATE_READY_UPCALL));

	brand->pending_upcall_requests--;

//	printk("%d-%ld\n", thd_get_id(uc), brand->pending_upcall_requests);

	dest = brand->stack_base[brand->stack_ptr].spd;
	assert(dest);
//	cos_net_try_packet(brand, &port);
//	upcall_setup(uc, dest, COS_UPCALL_BRAND_EXEC, port, 0, 0);
	upcall_setup(uc, dest, COS_UPCALL_BRAND_EXEC, 0, 0, 0);
	upcall_execute(uc, NULL, curr);

	cos_meas_event(COS_MEAS_BRAND_PEND_EXECUTE);
	cos_meas_event(COS_MEAS_FINISHED_BRANDS);

	return &uc->regs;
}

static struct pt_regs *brand_execution_completion(struct thread *curr)
{
	struct thread *prev, *brand = curr->thread_brand;
    	struct composite_spd *cspd = (struct composite_spd *)thd_get_thd_spdpoly(curr);
	
	assert((curr->flags & THD_STATE_ACTIVE_UPCALL) &&
	       !(curr->flags & THD_STATE_READY_UPCALL));
	assert(brand && cspd);
	spd_mpd_ipc_release(cspd);

	cos_meas_stats_end(COS_MEAS_STATS_UC_TERM_DELAY, 1);
	cos_meas_stats_end(COS_MEAS_STATS_UC_PEND_DELAY, 0);

	/* Immediately execute a pending upcall */
	if (brand->pending_upcall_requests) {
		return sched_tailcall_pending_upcall(curr, cspd);
	}

	/*
	 * Has the thread we preempted had scheduling activity since?
	 * If so, upcall into the root scheduler and ask it what to
	 * do.
	 */
	prev = curr->interrupted_thread;
	if (NULL == prev) {
		struct thd_sched_info *tsi;
		struct spd *dest;

		/* FIXME: do this to the most common scheduler */
		tsi = thd_get_sched_info(curr, 0);
		dest = tsi->scheduler;

		upcall_setup(curr, dest, COS_UPCALL_BRAND_COMPLETE, 0, 0, 0);
		upcall_execute(curr, NULL, cspd);

		cos_meas_event(COS_MEAS_BRAND_COMPLETION_UC);
		//cos_meas_event(COS_MEAS_FINISHED_BRANDS);
		return &curr->regs;
	}
	cos_meas_event(COS_MEAS_BRAND_SCHED_PREEMPTED);
	cos_meas_event(COS_MEAS_FINISHED_BRANDS);

	break_preemption_chain(curr);
	curr->sched_suspended = NULL;

	curr->flags &= ~THD_STATE_ACTIVE_UPCALL;
	curr->flags |= THD_STATE_READY_UPCALL;
	/* 
	 * FIXME: this should be more complicated.  If
	 * a scheduling decision has been made between
	 * when the upcall thread was scheduled, and
	 * now.  In such a case, the "previous
	 * preempted thread" could have already
	 * executed to completion, or some such.  In
	 * such a case (scheduling decision has been
	 * made to put the upcall thread to sleep),
	 * then the correct thing to do is to act like
	 * this thread has been killed (or yields, or
	 * something in between) for scheduling
	 * purposes (assuming that we don't have
	 * pending upcalls, which changes all of this.
	 *
	 * UPDATE: this has been dealt with by adding the
	 * BREAK_PREEMPTION_CHAIN flag to sched_cntl.
	 */
	switch_thread_context(curr, prev);

	/* This might not be true if we are a brand that was just
	 * branded by another thread.  That other thread just branded
	 * us, and wasn't preempted. */
//	if (prev->flags & THD_STATE_PREEMPTED) {
	remove_preempted_status(prev);
//	}
	update_sched_evts(prev, COS_SCHED_EVT_NIL, 
			  curr, COS_SCHED_EVT_BRAND_READY);

	return &prev->regs;
}

/**
 * Upcall interfaces:
 *
 * The current interfaces for upcalls rely on maintaining the state of
 * an execution path through components which is traversed in reverse
 * order by each upcall.  This mechanism will not work when components
 * are collapsed into larger protection domains as 1) the direct
 * function calls will not know where to execute (as the invocation
 * stack is kept in kernel) and 2) the kernel will not know in which
 * component an invocation is made from.
 *
 * When a thread is going to request a brand, it must make only
 * invocations via SDT (and will therefore not be on a fast-path).
 * When we are making brand upcalls, we might skip some spds in the
 * invocation stack of the brand thread due to MPD whereby multiple
 * spds are collapsed into one composite spd.
 */
struct thread *brand_next_thread(struct thread *brand, struct thread *preempted, int preempt);

//#define BRAND_UL_LATENCY

//#define BRAND_HW_LATENCY

#ifdef BRAND_HW_LATENCY

//#ifdef BRAND_UL_LATENCY
unsigned int glob_hack_arg;
#endif

extern void cos_syscall_brand_upcall(int spd_id, int thread_id_flags);
COS_SYSCALL struct pt_regs *cos_syscall_brand_upcall_cont(int spd_id, int thread_id_flags, int arg1, int arg2)
{
	struct thread *curr_thd, *brand_thd, *next_thd;
	struct spd *curr_spd;
	short int thread_id, flags;

//	static int first = 1;

	thread_id = thread_id_flags>>16;
	flags = thread_id_flags & 0x0000FFFF;

	curr_thd = thd_get_current();
	curr_spd = thd_validate_get_current_spd(curr_thd, spd_id);
	if (unlikely(NULL == curr_spd)) {
		printk("cos: component claimed in spd %d, but not\n", spd_id);
		goto upcall_brand_err;		
	}
	/*
	 * TODO: Check that the brand thread is on the same cpu as the
	 * current thread.
	 */
	brand_thd = thd_get_by_id(thread_id);
	if (unlikely(NULL == brand_thd)) {
		printk("cos: Attempting to brand thd %d - invalid thread.\n", thread_id);
		goto upcall_brand_err;
	}
/*	if (unlikely(thd_get_thd_spd(brand_thd) != curr_spd)) {
		printk("cos: attempted to make brand on thd %d, but from incorrect spd.\n", thread_id);
		goto upcall_brand_err;
	}
*/
	if (unlikely(!(brand_thd->flags & THD_STATE_BRAND) || !brand_thd->upcall_threads)) {
		printk("cos: cos_brand_upcall, thread %d not a brand\n", thread_id);
		goto upcall_brand_err;
	}

	/*
	 * FIXME: 1) reference counting taken care of????, 2) return 1
	 * if pending invocation?
	 */

#ifdef BRAND_UL_LATENCY
	glob_hack_arg = arg1;
#endif
	next_thd = brand_next_thread(brand_thd, curr_thd, 2);
	
	if (next_thd == curr_thd) {
		curr_thd->regs.eax = 0;
	} else {
		next_thd->regs.ebx = arg1;
		next_thd->regs.edi = arg2;
		curr_thd->regs.eax = 1;
	}
/* This to measure the cost of pending upcalls
	if (unlikely(first)) {
		brand_thd->pending_upcall_requests = 10000000;
		first = 0;
	}
*/
	return &next_thd->regs;

upcall_brand_err:
	curr_thd->regs.eax = -1;
	return &curr_thd->regs;
}

/*
 * TODO: Creating the thread in this function is a little
 * brain-damaged because now we are allocating threads without the
 * scheduler knowing it (shouldn't be allowed), and because there is a
 * limited number of threads, we could denial of service and no policy
 * could be installed in the system to stop us.  Solution: allow the
 * scheduler to create threads that aren't executed, but attached to
 * other threads (that make them), and these threads can later be used
 * to create brands and upcalls.  This allows the scheduler to control
 * the distrubution of threads, and essentially is a resource credit
 * to a principal where the resource here is a thread.
 *
 * FIXME: the way we record and do brand paths is incorrect currently.
 * It will work now, but not when we activate MPDs.  We need to make
 * sure that 1) all spd invocations are recorded when we are creating
 * a brand path, and 2) pointers are added only to the spds
 * themselves, not necessarily the spd's current protection domains as
 * we wish, when making upcalls to upcall into the most recent version
 * of the spd's protection domains.  This begs the question, when we
 * upcall_brand from a composite spd, how does the system know which
 * spd we are in, thus which to upcall into.  Solution: we must make
 * the upcall call be another capability which we can define a
 * user-level-cap for.  This is required anyway, as we need to have a
 * system-provided lookup for direct invocation.  When an upcall is
 * made, we walk the invocation stack till we find the current spd,
 * and upcall its return spd.  To improve usability, we should check
 * explicitely that when a brand is made, the chain of invocations
 * follows capabilities and doesn't skip spds due to mpds.
 */
static inline struct thread* verify_brand_thd(unsigned short int thd_id)
{
	struct thread *brand_thd;
	
	brand_thd = thd_get_by_id(thd_id);
	if (brand_thd == NULL) {
		printk("cos: cos_syscall_brand_cntl could not find thd_id %d to add thd to.\n", 
		       (unsigned int)thd_id);
		return NULL;
	}
	if (!(brand_thd->flags & THD_STATE_BRAND ||
	      brand_thd->flags & THD_STATE_HW_BRAND)) {
		printk("cos: cos_brand_cntl - adding upcall thd to thd %d that's not a brand\n",
		       (unsigned int)thd_id);
		return NULL;
	}
	
	return brand_thd;
}

/* static void print_thd_sched_structs(struct thread *t) */
/* { */
/* 	int i; */
/* 	struct thd_sched_info *tsi = t->sched_info; */

/* 	printk("cos: thread %d has scheduling info structures:\n", (unsigned int)thd_get_id(t)); */
/* 	for (i = 0 ; i < MAX_SCHED_HIER_DEPTH ; i++) { */
/* 		struct spd *s = tsi[i].scheduler; */

/* 		if (s) { */
/* 			printk("cos:\tdepth %d, scheduler %d, notification addr %x, offset %d\n", */
/* 			       i, (unsigned int)spd_get_index(s), (unsigned int)tsi[i].thread_notifications,  */
/* 			       (unsigned int)tsi[i].notification_offset); */
/* 		} */
/* 	} */
/* } */

COS_SYSCALL int cos_syscall_brand_cntl(int spd_id, int thd_id, int flags, spdid_t dest)
{
	struct thread *new_thd, *curr_thd;
	struct spd *curr_spd;

	curr_thd = thd_get_current();
	curr_spd = thd_validate_get_current_spd(curr_thd, spd_id);
	if (NULL == curr_spd) {
		printk("cos: component claimed in spd %d, but not\n", spd_id);
		return -1;		
	}

	new_thd = thd_alloc(curr_spd);
	if (NULL == new_thd) {
		return -1;
	}

	switch (flags) {
	case COS_BRAND_CREATE_HW:
		new_thd->flags |= THD_STATE_HW_BRAND;
		/* fall through */
	case COS_BRAND_CREATE: 
	{
		int depth;
		struct spd *s;
		struct thd_invocation_frame *f;

		/* the brand thread holds the invocation stack record: */
		memcpy(&new_thd->stack_base, &curr_thd->stack_base, sizeof(curr_thd->stack_base));
		new_thd->cpu_id = curr_thd->cpu_id;
		new_thd->flags |= THD_STATE_BRAND;
		s = spd_get_by_index(dest);
		if (NULL == s) {
			printk("cos: brand_cntl -- spd %d not found\n", dest);
			thd_free(new_thd);
			return -1;
		}
		if (-1 == (depth = thd_validate_spd_in_callpath(curr_thd, s))) {
//		if (depth > curr_thd->stack_ptr) {
			printk("cos: brand_cntl -- spd %d not found in stack for thread %d\n",
			       dest, thd_get_id(curr_thd));
			thd_free(new_thd);
			return -1;
		}
		new_thd->stack_ptr = curr_thd->stack_ptr - depth;
		f = &new_thd->stack_base[new_thd->stack_ptr];
		assert(thd_spd_in_composite(f->current_composite_spd, s));
		/* HACK: brands made past the first entry spd will break. */
		f->spd = s;

		copy_sched_info(new_thd, curr_thd);
		new_thd->flags |= THD_STATE_CYC_CNT;
		break;
	} 
	case COS_BRAND_ADD_THD:
	{
		struct thread *brand_thd = verify_brand_thd(thd_id);
		if (NULL == brand_thd) {
			thd_free(new_thd);
			return -1;
		}

		new_thd->flags = (THD_STATE_UPCALL | THD_STATE_READY_UPCALL);
		new_thd->thread_brand = brand_thd;
		new_thd->brand_inv_stack_ptr = brand_thd->stack_ptr;
		new_thd->upcall_threads = brand_thd->upcall_threads;
		brand_thd->upcall_threads = new_thd;

		copy_sched_info(new_thd, brand_thd);
		new_thd->flags |= THD_STATE_CYC_CNT;

		//print_thd_sched_structs(new_thd);
		break;
	}
	default:
		return -1;
	}

	return new_thd->thread_id;
}

struct thread *cos_timer_brand_thd, *cos_upcall_notif_thd;
#define NUM_NET_BRANDS 2
unsigned int active_net_brands = 0;
struct cos_brand_info cos_net_brand[NUM_NET_BRANDS];
struct cos_net_callbacks *cos_net_fns = NULL;

void cos_net_init(void)
{
	int i;
	
	active_net_brands = 0;
	for (i = 0 ; i < NUM_NET_BRANDS ; i++) {
		cos_net_brand[i].brand = NULL;
		cos_net_brand[i].brand_port = 0;
	}
}

struct cos_brand_info *cos_net_brand_info(struct thread *t)
{
	int i;

	for (i = 0 ; i < NUM_NET_BRANDS ; i++) {
		if (cos_net_brand[i].brand == t) {
			return &cos_net_brand[i];
		}
	}
	return NULL;
}

void cos_net_finish(void)
{
	int i;
	
	active_net_brands = 0;
	for (i = 0 ; i < NUM_NET_BRANDS ; i++) {
		if (cos_net_brand[i].brand) {
			if (!cos_net_fns || !cos_net_fns->remove_brand ||
			    cos_net_fns->remove_brand(&cos_net_brand[i])) {
				printk("cos: error deregistering net brand for port %d\n",
					cos_net_brand[i].brand_port);
			}
		}
		cos_net_brand[i].brand = NULL;
		cos_net_brand[i].brand_port = 0;
	}
}

void cos_net_register(struct cos_net_callbacks *cn_cb)
{
	assert(cn_cb->get_packet && cn_cb->create_brand);

	printk("cos: Registering networking callbacks @ %x\n", (unsigned int)cn_cb);
	cos_net_fns = cn_cb;
}

void cos_net_deregister(struct cos_net_callbacks *cn_cb)
{
	assert(cos_net_fns == cn_cb);

	printk("cos: Deregistering networking callbacks\n");
	cos_net_fns = NULL;
}

extern int host_attempt_brand(struct thread *brand);

void cos_net_prebrand(void)
{
	cos_meas_event(COS_MEAS_PACKET_RECEPTION);
}

extern int rb_retrieve_buff(struct thread *brand, int desired_len, 
			    void **found_buf, int *found_len);
extern int rb_setup(struct thread *brand, ring_buff_t *user_rb, ring_buff_t *kern_rb);

int cos_net_try_brand(struct thread *t, void *data, int len)
{
	void *buff;
	int l;

#ifdef BRAND_HW_LATENCY
	unsigned long long start;

	rdtscll(start);
	glob_hack_arg = (unsigned long)start;
#endif
	cos_meas_event(COS_MEAS_PACKET_BRAND);

	/* 
	 * If there is no room for the network buffer, then don't
	 * attempt the upcall.  This is analogous to not trying to
	 * raise an interrupt when there are no buffers to write into.
	 */
	if (rb_retrieve_buff(t, len, &buff, &l)) {
		cos_meas_event(COS_MEAS_PACKET_BRAND_FAIL);
		return -1;
	}
	cos_meas_event(COS_MEAS_PACKET_BRAND_SUCC);
	memcpy(buff, data, len);

	host_attempt_brand(t);
	
	return 0;
}

int cos_net_notify_drop(struct thread *brand)
{
	struct thread *uc;

	if (!brand) return -1;

	uc = brand->upcall_threads;
	if (uc) {
/*		if (uc->flags & THD_STATE_READY_UPCALL) {
			cos_meas_event(COS_MEAS_PENDING_HACK)
		}
*/
		if (brand->pending_upcall_requests == 0) {
			cos_meas_event(COS_MEAS_PENDING_HACK);
		}
		//update_thd_evt_state(uc, COS_SCHED_EVT_BRAND_PEND, 1);
	} else {
		return -1;
	}

	return 0;
}

/* 
 * Partially emulate a device here: Receive ring for holding buffers
 * to receive data into, and a synchronous call to transmit data.
 */
extern vaddr_t pgtbl_vaddr_to_kaddr(phys_addr_t pgtbl, unsigned long addr);
extern int user_struct_fits_on_page(unsigned long addr, unsigned int size);
/* assembly in ipc.S */
extern int cos_syscall_buff_mgmt(void);
COS_SYSCALL int cos_syscall_buff_mgmt_cont(int spd_id, void *addr, unsigned int thd_id, unsigned int len_op)
{
	/* 
	 * FIXME: To do this right, we would need to either 1) pin the
	 * buffer's pages into memory, or 2) interact closely with the
	 * network subsystem so that any memory pages the mem_man
	 * unmaps from the address space are removed from the network
	 * buffer lists too.  For 1, the pages might be pre-pinned at
	 * map time.
	 */
	struct spd *spd;
	vaddr_t kaddr = 0;
	unsigned short int option, len;

	option = (len_op & 0xFFFF);
	len = len_op >> 16;

	spd = thd_validate_get_current_spd(thd_get_current(), spd_id);
	if (!spd) {
		printk("cos: buff mgmt -- invalid spd, %d for thd %d\n", 
		       spd_id, thd_get_id(thd_get_current()));
		return -1;
	}

	if (unlikely(COS_BM_XMIT != option &&
		     0 == (kaddr = pgtbl_vaddr_to_kaddr(spd->spd_info.pg_tbl, (unsigned long)addr)))) {
		printk("cos: buff mgmt -- could not find kernel address for %p in spd %d\n",
		       addr, spd_id);
		return -1;
	}
	
	switch(option) {
	/* Transmit the data buffer */
	case COS_BM_XMIT:
	{
		struct cos_net_xmit_headers *h = spd->cos_net_xmit_headers;
		int gather_buffs = 0, i, tot_len = 0;
		struct gather_item gi[XMIT_HEADERS_GATHER_LEN];

		if (unlikely(NULL == h)) return -1;
		gather_buffs = h->gather_len;
		if (unlikely(gather_buffs > XMIT_HEADERS_GATHER_LEN)) {
			printk("cos buff mgmt -- gather list length %d too large.", gather_buffs);
			return -1;
		}
		/* Check that each of the buffers in the gather list are legal */
		for (i = 0 ; i < gather_buffs ; i++) {
			struct gather_item *user_gi = &h->gather_list[i];
			tot_len += user_gi->len;

			if (!user_struct_fits_on_page((unsigned long)user_gi->data, user_gi->len)) {
				printk("cos: buff mgmt -- buffer address  %p does not fit onto page\n", user_gi->data);
				return -1;
			}
			kaddr = pgtbl_vaddr_to_kaddr(spd->spd_info.pg_tbl, (unsigned long)user_gi->data);
			if (!kaddr) {		    
				printk("cos: buff mgmt -- could not find kernel address for %p in spd %d\n",
				       user_gi->data, spd_id);
				return -1;
			}

			gi[i].data = (void*)kaddr;
			gi[i].len  = user_gi->len;
		}

		/* Transmit! */
		if (likely(cos_net_fns && cos_net_fns->xmit_packet && h)) {
			cos_meas_event(COS_MEAS_PACKET_XMIT);
			return cos_net_fns->xmit_packet(h->headers, h->len, gi, gather_buffs, tot_len);
		}
		break;
	}
	case COS_BM_XMIT_REGION:
	{
		if (len != sizeof(struct cos_net_xmit_headers)) {
			printk("cos: buff mgmt -- xmit header region of length %d, expected %d.\n",
			       len, sizeof(struct cos_net_xmit_headers));
			return -1;
		}
		if (!user_struct_fits_on_page((unsigned long)addr, len)) {
			printk("cos: buff mgmt -- xmit headers address %p w/ len %d does not fit onto page\n", 
			       addr, len);
			return -1;
		}
		/* FIXME: pin page in memory */
		spd->cos_net_xmit_headers = (struct cos_net_xmit_headers*)kaddr;

		break;
	}
	/* Set the location of a user-level ring buffer */
	case COS_BM_RECV_RING:
	{
		struct thread *b;

		/*
		 * Currently, the ring buffer must be aligned on a
		 * page, and be a page long
		 */
		if ((unsigned long)addr & ~PAGE_MASK || len != PAGE_SIZE) {
			printk("cos: buff mgmt -- recv ring @ %p (%d) not on page boundary.\n", addr, len);
			return -1;
		}
		if (NULL == (b = thd_get_by_id(thd_id))) {
			printk("cos: buff mgmt could not find brand thd %d.\n", 
		       (unsigned int)thd_id);
			return -1;
		}
		if (b->flags & THD_STATE_UPCALL) {
			assert(b->thread_brand);
			b = b->thread_brand;
		}
		if (!(b->flags & THD_STATE_BRAND ||
		      b->flags & THD_STATE_HW_BRAND)) {
			printk("cos: buff mgmt attaching ring buffer to thread not a brand: %d\n",
			       (unsigned int)thd_id);
			return -1;
		}
		if (thd_get_thd_spd(b) != spd) {
			printk("cos: buff mgmt trying to set buffer for brand not in curr spd");
			return -1;
		}
		/* FIXME: pin the page in memory. */
		if (rb_setup(b, (ring_buff_t*)addr, (ring_buff_t*)kaddr)) {
			printk("cos: buff mgmt -- could not setup the ring buffer.\n");
			return -1;
		}
		break;
	}
	default:
		printk("cos: buff mgmt -- unknown option %d.\n", option);
		return -1;
	}
	return 0;
}

/*
 * This is a bandaid currently.  This syscall should really be 
 * replaced by something a little more subtle and more closely related
 * to the APIC and timer hardware, rather than the device in general.
 */
COS_SYSCALL int cos_syscall_brand_wire(int spd_id, int thd_id, int option, int data)
{
	struct thread *curr_thd, *brand_thd;
	struct spd *curr_spd;

	curr_thd = thd_get_current();
	curr_spd = thd_validate_get_current_spd(curr_thd, spd_id);
	if (NULL == curr_spd) {
		printk("cos: wiring brand to hardware - component claimed in spd %d, but not\n", spd_id);
		return -1;		
	}

	brand_thd = verify_brand_thd(thd_id);
	if (NULL == brand_thd || !(brand_thd->flags & THD_STATE_HW_BRAND)) {
		printk("cos: wiring brand to hardware - thread %d not brand thd\n",
		       (unsigned int)thd_id);
		return -1;
	}

	switch (option) {
	case COS_HW_TIMER:
		cos_timer_brand_thd = brand_thd;
		
		break;
	case COS_HW_NET:
		if (active_net_brands >= NUM_NET_BRANDS || !cos_net_fns) {
			printk("cos: Too many network brands.\n\n");
			return -1;
		}

		cos_net_brand[active_net_brands].brand_port = (unsigned short int)data;
		cos_net_brand[active_net_brands].brand = brand_thd;
		if (!cos_net_fns ||
		    !cos_net_fns->create_brand || 
		    cos_net_fns->create_brand(&cos_net_brand[active_net_brands])) {
			printk("cos: could not create brand in networking subsystem\n");
			return -1;
		}
		active_net_brands++;

		break;
	case COS_UC_NOTIF:
		cos_upcall_notif_thd = brand_thd;

		break;
	default:
		return -1;
	}

	return 0;
}

/*
 * verify that truster does in fact trust trustee
 */
static int verify_trust(struct spd *truster, struct spd *trustee)
{
	unsigned short int cap_no, max_cap, i;

	cap_no = truster->cap_base;
	max_cap = truster->cap_range + cap_no;

	for (i = cap_no ; i < max_cap ; i++) {
		if (invocation_capabilities[i].destination == trustee) {
			return 0;
		}
	}

	return -1;
}

/* 
 * I HATE this call...do away with it if possible.  But we need some
 * way to jump-start processes, let schedulers keep track of their
 * threads, and be notified when threads die.
 *
 * NOT performance sensitive: used to kick start spds and give them
 * active entities (threads).
 */
extern void cos_syscall_upcall(void);
COS_SYSCALL int cos_syscall_upcall_cont(int this_spd_id, int spd_id, struct pt_regs **regs/*vaddr_t *inv_addr*/)
{
	struct spd *dest, *curr_spd;
	struct thread *thd;

	assert(regs);
	*regs = NULL;

	dest = spd_get_by_index(spd_id);
	thd = thd_get_current();
	curr_spd = thd_validate_get_current_spd(thd, this_spd_id);

	if (NULL == dest || NULL == curr_spd) {
		printk("cos: upcall attempt failed - dest_spd = %d, curr_spd = %d.\n",
		       dest     ? spd_get_index(dest)     : 0, 
		       curr_spd ? spd_get_index(curr_spd) : 0);
		return -1;
	}

	/*
	 * Check that we are upcalling into a service that explicitely
	 * trusts us (i.e. that the current spd is allowed to upcall
	 * into the destination.)
	 */
	if (verify_trust(dest, curr_spd)) {
		printk("cos: upcall attempted from %d to %d without trust relation.\n",
		       spd_get_index(curr_spd), spd_get_index(dest));
		return -1;
	}

	/* 
	 * Is a parent scheduler granting a thread to a child
	 * scheduler?  If so, we need to add the child scheduler to
	 * the thread's scheduler info list, UNLESS this thread is
	 * already owned by another child scheduler.
	 *
	 * FIXME: remove this later as it adds redundance with the
	 * sched_cntl call, reducing orthogonality of the syscall
	 * layer.
	 */
#ifdef NIL
	if (dest->parent_sched == curr_spd) {
		struct thd_sched_info *tsi;

		tsi = thd_get_sched_info(thd, curr_spd->sched_depth+1);
		if (NULL == tsi->scheduler) {
			tsi->scheduler = dest;
		}
	}
#endif
	open_close_spd(dest->composite_spd, curr_spd->composite_spd); 

	spd_mpd_ipc_release((struct composite_spd *)thd_get_thd_spdpoly(thd));//curr_spd->composite_spd);
	//spd_mpd_ipc_take((struct composite_spd *)dest->composite_spd);

	/* FIXME: use upcall_(setup|execute) */
	upcall_setup(thd, dest, COS_UPCALL_BOOTSTRAP, 0, 0, 0);
	*regs = &thd->regs;

	cos_meas_event(COS_MEAS_UPCALLS);

	return thd_get_id(thd);
}


/****************** begin event notification functions ******************/

/* 
 * Update the linked list in the shared data page between the kernel
 * and the scheduler as an event has been added.  It will attempt to
 * add the event to the end of the list.  If this event happened for a
 * thread that is already in that linked list, then don't modify the
 * list (to avoid cycles/trees).
 */
static int update_evt_list(struct thd_sched_info *tsi)
{
	unsigned short int prev_evt, this_evt;
	struct cos_sched_events *evts;
	struct spd *sched;
	struct cos_sched_data_area *da;
	
	assert(tsi);
	assert(tsi->scheduler);
	assert(tsi->scheduler->kern_sched_shared_page);

	sched = tsi->scheduler;
	/* if tsi->scheduler, then all of this should follow */
	da = sched->kern_sched_shared_page;
	/* 
	 * Here we want to prevent a race condition:
	 *
	 * t1 executes through the scheduler and sets
	 * da->cos_next.next_thd_id to the next thread it believes it
	 * should schedule.  Then it is preempted before it can call
	 * switch_thread.  An event occurs (upcall, woken thread,
	 * etc.) which changes the scheduling decision.  t1 is run
	 * again, but it doesn't know about the event, so it switches
	 * to the wrong thread.  Prevent this by setting next_thd_id
	 * to 0 here, and check for that case in switch_thread.
	 */
	da->cos_next.next_thd_id = 0;

	evts = da->cos_events;
	prev_evt = sched->prev_notification;
	this_evt = tsi->notification_offset;
	if (unlikely(prev_evt >= NUM_SCHED_EVTS ||
		     this_evt >= NUM_SCHED_EVTS ||
		     this_evt == 0)) {
		printk("cos: events %d and %d out of range!\n", prev_evt, this_evt);
		return -1;
	}
	/* so long as we haven't already processed this event, and it
	 * is not part of the linked list of events, then add it */
	if (prev_evt != this_evt && 
	    COS_SCHED_EVT_NEXT(&evts[this_evt]) == 0) {
		if (unlikely(COS_SCHED_EVT_NEXT(&evts[prev_evt]) != 0)) {
			printk("cos: user-level scheduler %d not following evt protocol for evt %d\n",
			       (unsigned int)spd_get_index(sched), (unsigned int)prev_evt);
			/*
			 * FIXME: how should we notify it?  Should we
			 * notify it?  What to do here?
			 */
		}
		COS_SCHED_EVT_NEXT(&evts[prev_evt]) = this_evt;
		sched->prev_notification = this_evt;
	}
	
	return 0;
}

static inline void update_thd_evt_state(struct thread *t, int flags, int update_list)
{
	int i;
	struct thd_sched_info *tsi;

	assert(flags != COS_SCHED_EVT_NIL);

	for (i = 0 ; i < MAX_SCHED_HIER_DEPTH ; i++) {
		tsi = thd_get_sched_info(t, i);
		if (NULL != tsi->scheduler && tsi->thread_notifications) {
			switch(flags) {
			case COS_SCHED_EVT_BRAND_PEND:
				cos_meas_event(COS_MEAS_EVT_PENDING);
				break;
			case COS_SCHED_EVT_BRAND_READY:
				cos_meas_event(COS_MEAS_EVT_READY);
				break;
			case COS_SCHED_EVT_BRAND_ACTIVE:
				cos_meas_event(COS_MEAS_EVT_ACTIVE);
				break;
			}

			/* 
			 * FIXME: should a pending flag update
			 * override an activate one????
			 */
			COS_SCHED_EVT_FLAGS(tsi->thread_notifications) = flags;
			/* handle error conditions of list manip here??? */
			if (update_list) update_evt_list(tsi);
		}
	}
	
	return;
}

static inline void update_thd_evt_cycles(struct thread *t, unsigned long consumption)
{
	struct thd_sched_info *tsi;
	int i;

	for (i = 0 ; i < MAX_SCHED_HIER_DEPTH ; i++) {
		tsi = thd_get_sched_info(t, i);
		if (NULL != tsi->scheduler && tsi->thread_notifications) {
			struct cos_sched_events *se = tsi->thread_notifications;
			u32_t p, n;

			n = p = se->cpu_consumption;
			n += consumption;
			if (n < p) se->cpu_consumption = ~0UL; /* prevent overflow */
			else       se->cpu_consumption = n;
			update_evt_list(tsi);
		}
	}
}

void update_sched_evts(struct thread *new, int new_flags, 
		       struct thread *prev, int prev_flags)
{
	int update_list = 1;

	assert(new && prev);

	/* 
	 * - if either thread has cyc_cnt set, do rdtsc (this 
	 *   is expensive, ~80 cycles, so avoid it if possible)
	 * - if prev has cyc_cnt set, do sched evt cycle update
	 * - if new_flags, do sched evt flags update on new
	 * - if prev_flags, do sched evt flags update on prev
	 */
	if ((new->flags | prev->flags) & THD_STATE_CYC_CNT) {
		unsigned long last;

		last = cycle_cnt;
		rdtscl(cycle_cnt);
		if (prev->flags & THD_STATE_CYC_CNT) {
			update_thd_evt_cycles(prev, cycle_cnt - last);
			update_list = 0;
		}
	}
	
	if (new_flags != COS_SCHED_EVT_NIL) {
		update_thd_evt_state(new, new_flags, 1);
	}
	if (prev_flags != COS_SCHED_EVT_NIL) {
		update_thd_evt_state(prev, prev_flags, update_list);
	}

	return;
}

/****************** end event notification functions ******************/

/************** functions for parsing async set urgencies ************/

static inline int most_common_sched_depth(struct thread *t1, struct thread *t2)
{
	int i;

	/* root scheduler had better be the same */
	assert(thd_get_depth_sched(t1, 0) == thd_get_depth_sched(t2, 0));

	for (i = 1 ; i < MAX_SCHED_HIER_DEPTH ; i++) {
		struct spd *s1, *s2;

		s1 = thd_get_depth_sched(t1, i);
		s2 = thd_get_depth_sched(t2, i);

		/* If the scheduler's diverge, previous depth is most common */
		if (!s1 || s1 != s2) {
			return i-1;
		}
	}

	return MAX_SCHED_HIER_DEPTH-1;
}

/* 
 * Not happy with the complexity of this function...
 *
 * A thread has made a brand and wishes to execute an upcall.  Here we
 * decide if that upcall should be made now based on the currently
 * executing thread, or if it should be postponed for schedulers to
 * deal with later.
 * 
 * There are 4 sets of schedulers, a - the most common scheduler
 * between uc and preempted, >a - the set of schedulers of more
 * authority than a, <a_uc - the set of schedulers that own uc of less
 * authority than a, and <a_pre - the set of schedulers that own
 * preempted that have less authority than a.
 *
 * If upcall is already active, signal that it should be not run (ret 0)
 * otherwise notify all schedulers in (<a_uc + a + >a) that upcall has
 * awakened and
 * if the urgency of upcall in a is higher (lower numerically) than
 *    prev in a
 *    - check forall s in (<a_uc + a + >a) that upcall is not disabled.
 *    - return 1 to signal that upcall should be executed.
 */
int brand_higher_urgency(struct thread *upcall, struct thread *prev)
{
	int d;
	u16_t u_urg, p_urg;

	assert(upcall->thread_brand && upcall->flags & THD_STATE_UPCALL);

	d = most_common_sched_depth(upcall, prev);
	/* FIXME FIXME FIXME FIXME FIXME FIXME FIXME this is a stopgap
	 * measure.  I don't know hy these are null when we are
	 * shutting down the system but still get a packet.  This will
	 * shut it up for now.
	 */
	if (!thd_get_sched_info(upcall, d)->thread_notifications ||
	    !thd_get_sched_info(prev, d)->thread_notifications) {
		printk("cos: skimping on brand metadata maintenance, and returning.\n");
		return 0;
	}
	u_urg = thd_get_depth_urg(upcall, d);
	p_urg = thd_get_depth_urg(prev, d);
	/* We should not run the upcall if it doesn't have more
	 * urgency, remember here that higher numerical values equals
	 * less importance. */
	if (u_urg < p_urg) {
		update_sched_evts(upcall, COS_SCHED_EVT_BRAND_ACTIVE, 
				  prev, COS_SCHED_EVT_NIL);
		return 1;
	} else {
		update_thd_evt_state(upcall, COS_SCHED_EVT_BRAND_ACTIVE, 1);
		return 0;
	}
}

/* 
 * This does NOT release the composite spd reference of the preempted
 * thread, as you might expect.
 *
 * preempt = 0 if you don't want any of the preemption lists to be
 * updated, and if you don't want the preempted thread to be set as
 * PREEMPTED.  Pass in 1 if you want both of those things.  2 if you
 * only want the lists to be updated.  I know...this needs to change:
 * hurried evolution.
 *
 * execution = 1 if you want this to possibly lead to the upcall being
 * executed.  Otherwise, it won't be, even if the schedulers deem it
 * to be most important.
 */
struct thread *brand_next_thread(struct thread *brand, struct thread *preempted, int preempt)
{
	/* Assume here that we only have one upcall thread */
	struct thread *upcall = brand->upcall_threads;

	assert(brand->flags & (THD_STATE_BRAND|THD_STATE_HW_BRAND));
	assert(upcall && upcall->thread_brand == brand);

	/* 
	 * If the upcall is already active, the scheduler's already
	 * know what they're doing, and has chosen to run preempted.
	 * Don't second guess it.
	 *
	 * Do the same if upcall threads haven't been added to this
	 * brand.
	 */
//#define MEAS_LESSER_URG
#ifndef MEAS_LESSER_URG
	if (upcall->flags & THD_STATE_ACTIVE_UPCALL) {
		assert(!(upcall->flags & THD_STATE_READY_UPCALL));
		cos_meas_event(COS_MEAS_BRAND_PEND);
		cos_meas_stats_start(COS_MEAS_STATS_UC_PEND_DELAY, 0);
		brand->pending_upcall_requests++;
//		printk("%d+%ld\n", thd_get_id(upcall), brand->pending_upcall_requests);

		/* 
		 * This is an annoying hack to make sure we notify the
		 * scheduler that the upcall is active.  Because
		 * upcall notifications are edge triggered, if for
		 * some reason the scheduler misses one of the
		 * notifications, this can server as a reminder.
		 */
//		update_thd_evt_state(upcall, COS_SCHED_EVT_BRAND_PEND, 1);
//		cos_meas_event(COS_MEAS_PENDING_HACK);

		//printk("cos: upcall thread @ %x or %x, current is %d\n", 
		//       upcall->regs.eip, upcall->regs.edx, thd_get_id(thd_get_current()));
		return preempted;
	}
#endif
	assert(upcall->flags & THD_STATE_READY_UPCALL);
//	cos_net_try_packet(brand, &port);

//#ifdef (BRAND_UL_LATENCY || BRAND_HW_LATENCY)
#ifdef BRAND_HW_LATENCY
	upcall_setup(upcall, brand->stack_base[brand->stack_ptr].spd, 
		     COS_UPCALL_BRAND_EXEC, port, glob_hack_arg, 0);
#else
	upcall_setup(upcall, brand->stack_base[brand->stack_ptr].spd, 
//		     COS_UPCALL_BRAND_EXEC, port, 0, 0);
		     COS_UPCALL_BRAND_EXEC, 0, 0, 0);
#endif
	upcall->flags |= THD_STATE_ACTIVE_UPCALL;
	upcall->flags &= ~THD_STATE_READY_UPCALL;

//#define BRAND_SCHED_UPCALL
#ifdef BRAND_SCHED_UPCALL
	printk("cos: invoking the notification upcall!\n");

	if (!cos_upcall_notif_thd) {
		printk("cos: cannot make upcalls until you make the notification thread!");
		return preempted;
	}
	assert(preempted != cos_upcall_notif_thd->upcall_threads);

	cos_upcall_notif_thd->upcall_threads->flags |= THD_STATE_ACTIVE_UPCALL;
	cos_upcall_notif_thd->upcall_threads->flags &= ~THD_STATE_READY_UPCALL;
	upcall_setup(cos_upcall_notif_thd->upcall_threads, 
		     thd_get_sched_info(upcall, 0)->scheduler,
		     COS_UPCALL_BRAND_EXEC, 0, 0, 0);
	upcall_execute(cos_upcall_notif_thd->upcall_threads, preempted, 
		       (struct composite_spd*)thd_get_thd_spdpoly(preempted));
	update_sched_evts(cos_upcall_notif_thd->upcall_threads, COS_SCHED_EVT_BRAND_ACTIVE, 
			  preempted, COS_SCHED_EVT_NIL);
	update_thd_evt_state(upcall, COS_SCHED_EVT_BRAND_ACTIVE, 1);

	return cos_upcall_notif_thd->upcall_threads;
#endif	

	cos_meas_stats_start(COS_MEAS_STATS_UC_EXEC_DELAY, 1);
	cos_meas_stats_start(COS_MEAS_STATS_UC_TERM_DELAY, 1);
	cos_meas_stats_start(COS_MEAS_STATS_UC_PEND_DELAY, 1);
	if (brand_higher_urgency(upcall, preempted)) {
		if (unlikely(preempted->flags & THD_STATE_PREEMPTED)) {
			printk("cos: WTF - preempted thread %d preempted, upcall %d.\n", 
			       thd_get_id(preempted), thd_get_id(upcall));
			return preempted;
		}
		if (unlikely(preempted->preempter_thread != NULL)) {
			printk("cos: WTF - preempter thread pointer of preempted thread %d not null, upcall %d.\n",
			       thd_get_id(preempted), thd_get_id(upcall));
			return preempted;
		}
		if (likely(preempt)) {
			assert((preempted->flags & THD_STATE_PREEMPTED) == 0);
			assert(preempted->preempter_thread == NULL);

			/* This dictates how the registers for
			 * preempted are restored later. */
			if (preempt == 1) preempted->flags |= THD_STATE_PREEMPTED;
			preempted->preempter_thread = upcall;
			upcall->interrupted_thread = preempted;
		} else {
			upcall->interrupted_thread = NULL;
		}

		//printk("%d uc\n", thd_get_id(upcall));

		upcall_execute(upcall, preempted, (struct composite_spd*)thd_get_thd_spdpoly(preempted));
		/* actually setup the brand/upcall to happen here? */
		cos_meas_event(COS_MEAS_BRAND_UC);
		cos_meas_stats_end(COS_MEAS_STATS_UC_EXEC_DELAY, 1);
		return upcall;
	} else {
		/* 
		 * If another upcall is what we attempted to preempt,
		 * we might have a higher priority than the preempted
		 * thread of that upcall.  Thus we must break its
		 * preemption chain.
		 */
		if (preempted->flags & THD_STATE_ACTIVE_UPCALL) {
			break_preemption_chain(preempted);
		}

//		printk("%d w\n", thd_get_id(upcall));
	}

	cos_meas_event(COS_MEAS_BRAND_DELAYED);
	return preempted;
}

/************** end functions for parsing async set urgencies ************/

COS_SYSCALL int cos_syscall_sched_cntl(int spd_id, int operation, int thd_id, long option)
{
	struct thread *thd;
	struct spd *spd;

	thd = thd_get_current();
	spd = thd_validate_get_current_spd(thd, spd_id);
	if (NULL == spd) {
		printk("cos: component claimed in spd %d, but not\n", spd_id);
		return -1;
	}

	if (spd->sched_depth < 0) {
		printk("cos: spd %d called sched_cntl, but not a scheduler.\n", spd_id);
		return -1;
	}
/* Is this necessary??
	tsi = thd_get_sched_info(thd, spd->sched_depth);
	if (tsi->scheduler != spd) {
		printk("cos: spd %d @ depth %d attempting sched_cntl not a scheduler of thd %d (%x != %x).\n",
		       spd_get_index(spd), spd->sched_depth, thd_get_id(thd), (unsigned int)tsi->scheduler, (unsigned int)spd);
		return -1;
	}
*/

	switch(operation) {
	case COS_SCHED_EVT_REGION:
		/* 
		 * Set the event regions for this thread in
		 * user-space.  Make sure that the current scheduler
		 * has scheduling capabilities for this thread, and
		 * that the optional argument falls within the
		 * scheduler notification page.
		 */
		break;
	case COS_SCHED_THD_EVT:
	{
		long idx = option;
		struct cos_sched_events *evts, *this_evt;
		struct thd_sched_info *tsi;
		struct thread *thd;

		thd = thd_get_by_id(thd_id);
		if (!thd) {
			printk("cos: thd id %d passed into register event %d invalid.\n",
			       (unsigned int)thd_id, (unsigned int)idx);
			return -1;
		}
/* 		if (thd->flags & THD_STATE_UPCALL) { */
/* 			assert(thd->thread_brand); */
/* 			/\*  */
/* 			 * Set for all upcall thread associated with a */
/* 			 * brand, starting from the brand */
/* 			 *\/ */
/* 			thd = thd->thread_brand; */
/* 		} */

		tsi = thd_get_sched_info(thd, spd->sched_depth);
		if (tsi->scheduler != spd) {
			printk("cos: spd %d not the scheduler of %d to associate evt %d.\n",
			       spd_get_index(spd), (unsigned int)thd_id, (unsigned int)idx);
			return -1;
		}

		if (idx >= NUM_SCHED_EVTS || idx == 0) {
			printk("cos: invalid thd evt index %d for scheduler %d\n", 
			       (unsigned int)idx, (unsigned int)spd_id);
			return -1;
		}

		evts = spd->kern_sched_shared_page->cos_events;
		this_evt = &evts[idx];
		tsi->thread_notifications = this_evt;
		tsi->notification_offset = idx;
		COS_SCHED_EVT_NEXT(this_evt) = 0;
		COS_SCHED_EVT_FLAGS(this_evt) = 0;
		this_evt->cpu_consumption = 0;

		if (thd->flags & THD_STATE_BRAND) {
			struct thread *t = thd->upcall_threads;

			while (t) {
				copy_sched_info(t, thd);
				t = t->upcall_threads;
			}
		}

		//print_thd_sched_structs(thd);
		break;
	}
	case COS_SCHED_GRANT_SCHED:
	case COS_SCHED_REVOKE_SCHED:
	{
		/*
		 * Permit a child scheduler the capability to schedule
		 * the thread, or remove that capability.  Assumes
		 * that 1) this spd is a scheduler that has the
		 * capability to schedule the thread, 2) the target
		 * spd is a scheduler that is a child of this
		 * scheduler.
		 */ 
		struct thd_sched_info *child_tsi;
		struct thread *target_thd = thd_get_by_id(thd_id);
		struct spd *child = spd_get_by_index((int)option);
		int i;
		
		if (NULL == target_thd         || 
		    NULL == child              || 
		    spd != child->parent_sched ||
		    !thd_scheduled_by(target_thd, spd)) {
			printk("cos: Could not give privs for sched %d to thd %d from sched %d.\n",
			       (unsigned int)option, (unsigned int)thd_id, (unsigned int)spd_id);
			return -1;
		}
		
		child_tsi = thd_get_sched_info(target_thd, child->sched_depth);

		if (COS_SCHED_GRANT_SCHED == operation) {
			child_tsi->scheduler = child;
		} else if (COS_SCHED_REVOKE_SCHED == operation) {
			if (child_tsi->scheduler != child) {
				printk("cos: cannot remove privs when they aren't had\n");
				return -1;
			}

			child_tsi->scheduler = NULL;
		}
		/*
		 * revoke all schedulers that are decendents of the
		 * child.
		 */
		for (i = child->sched_depth+1 ; i < MAX_SCHED_HIER_DEPTH ; i++) {
			child_tsi = thd_get_sched_info(target_thd, i);
			child_tsi->scheduler = NULL;
		}
	}
	case COS_SCHED_BREAK_PREEMPTION_CHAIN:
	{
		/* 
		 * This call is simple: make it so that when the
		 * current thread (presumably an upcall) completes,
		 * don't automatically switch to the preempted thread,
		 * instead make an upcall into the scheduler.
		 */
		break_preemption_chain(thd);
		break;
	}
	default:
		printk("cos: cos_sched_cntl illegal operation %d.\n", operation);
		return -1;
	}

	return 0;
}

/*
 * Assume spd \in cspd.  Remove spd from cspd and add it to new1. Add
 * all remaining spds in cspd to new2.  If new2 == NULL, and cspd's
 * refcnt == 1, then we can just remove spd from cspd, and use cspd as
 * the new2 composite spd.  Returns 0 if the two new composites are
 * populated, -1 if there is an error, and 1 if we instead just reuse
 * the composite passed in by removing the spd from it (requires, of
 * course that cspd ref_cnt == 1, so that its mappings can change
 * without effecting any threads).  This is common because when we
 * split and merge, we will create a composite the first time for the
 * shrinking composite, but because it won't have active threads, that
 * composite can simply be reused by removing any further spds split
 * from it.
 */
static int mpd_split_composite_populate(struct composite_spd *new1, struct composite_spd *new2, 
					struct spd *spd, struct composite_spd *cspd)
{
	struct spd *curr;
	int remove_mappings;

	assert(cspd && spd_is_composite(&cspd->spd_info));
	assert(new1 && spd_is_composite(&new1->spd_info));
	assert(spd && spd_is_member(spd, cspd));
	assert(new1 != cspd);

	remove_mappings = (NULL == new2);
	spd_composite_remove_member(spd, remove_mappings);

	if (spd_composite_add_member(new1, spd)) {
		printk("cos: could not add member to new spd in split.\n");
		goto err_adding;
	}

	/* If the cspd is updated by removing the spd, and that spd
	 * has been added to the new composite spd, new1, we're
	 * done */
	if (NULL == new2) {
		assert(cos_ref_val(&cspd->spd_info.ref_cnt) == 1);
		return 1;
	}

	/* aliasing will mess everything up here */
	assert(new1 != new2);

	while (cspd->members) {
		curr = cspd->members;
		if (spd_composite_move_member(new2, curr)) {
			printk("cos: could not add spd to new composite in split.\n");
			goto err_adding;
		}
		assert(cspd->members != curr);
	}

	return 0;
 err_adding:
	return -1;
}

/*
 * Given a composite spd, c and a spd, s within it, split the spd out
 * of the composite, making two composite spds, c1 and c2.  c =
 * union(c1, c2), c\{s} = c1, {s} = c2.  This will create two
 * composite spds (c1 and c2) and will depricate and release (possibly
 * free) the preexisting composite c.  This method will reset all
 * capabilities correctly.
 */
static int mpd_split(struct composite_spd *cspd, struct spd *spd, short int *new, short int *old)
{
	short int d1, d2;
	struct composite_spd *new1, *new2;
	int ret = -1;

	assert(!spd_mpd_is_depricated(cspd));
	assert(spd_is_composite(&cspd->spd_info));
	assert(spd_is_member(spd, cspd));
	assert(spd_composite_num_members(cspd) > 1);

	d1 = spd_alloc_mpd_desc();
	if (d1 < 0) {
		printk("cos: could not allocate first mpd descriptor for split operation.\n");
		goto end;
	}
	new1 = spd_mpd_by_idx(d1);

	/*
	 * This condition represents the optimization whereby we wish
	 * to reuse the cspd instead of making a new one.  See the
	 * comment above mpd_composite_populate.  If we can reuse the
	 * current cspd by shrinking it rather than deleting it and
	 * allocating a new composite, then do it.
	 *
	 * This is a common case, e.g. when continuously moving spds
	 * from one cspd to another, but not in many other cases.
	 *
	 * It might be very possible to do this optimization when the
	 * reference count is > 1 because there is one subordinated
	 * domain, but that is the only (additional reference).
	 * Probably not worth adding in the logic for this.
	 */
	if (1 == cos_ref_val(&cspd->spd_info.ref_cnt)) {
		if (mpd_split_composite_populate(new1, NULL, spd, cspd) != 1) {
			ret = -1;
			goto err_d2;
		}
		*new = d1;
		*old = spd_mpd_index(cspd);

		cos_meas_event(COS_MPD_SPLIT_REUSE);

		ret = 0;
		goto end;
	}

	/* ...otherwise, we must allocate the other composite spd, and
	 * populate both of them */
	d2 = spd_alloc_mpd_desc();
	if (d2 < 0) {
		printk("cos: could not allocate second mpd descriptor for split operation.\n");
		goto err_d2;
	}
	new2 = spd_mpd_by_idx(d2);
	assert(new1 && new2);
	
	if (mpd_split_composite_populate(new1, new2, spd, cspd)) {
		printk("cos: populating two new cspds failed while splitting.\n");
		goto err_adding;
	}
	*new = d1;
	*old = d2;
	/* depricate the composite spd so that it cannot be used
	 * anymore from any user-level interfaces */
	spd_mpd_depricate(cspd);
	assert(!spd_mpd_is_depricated(new1) && !spd_mpd_is_depricated(new2));
	ret = 0;

	goto end;
	
 err_adding:
	spd_mpd_depricate(new2);
 err_d2:
	spd_mpd_depricate(new1);
 end:
	return ret;
}

/*
 * We want to subordinate one of the composite spds _only if_ we
 * cannot immediately free both the page tables (which subordination
 * does), but also the composite spd.  Thus, if one of the composites
 * can be freed, send it to be subordinated as it will be freed
 * immediately.  Secondarily, return the composite with fewer
 * components in it so there are less components to iterate through
 * when adding their mappings to the other composite.
 *
 * Let me make it explicit: First we are trying to save memory, then
 * processing time.  This might not be the appropriate long-term
 * prioritization, but only empirical studies will show.
 */
static inline struct composite_spd *get_spd_to_subordinate(struct composite_spd *c1, 
							   struct composite_spd *c2)
{
	int members1, members2;

	if (1 == cos_ref_val(&c1->spd_info.ref_cnt) &&
	    1 != cos_ref_val(&c2->spd_info.ref_cnt)) return c1;

	members1 = spd_composite_num_members(c1);
	members2 = spd_composite_num_members(c2);
	assert(members1 > 0 && members2 > 0);
	if (members1 < members2) return c1;
	return c2;
}

/*
 * Move all of the components from other to dest, thus merging the two
 * composite spds into one: dest.  This includes adding to the pg_tbl
 * of dest, all components in other.  The first version of this
 * function will have us actually releasing the other cspd to be
 * collected when not in use anymore.  This can result in the page
 * table of other sticking around for possibly a long time.  The
 * second version (calling spd_mpd_make_subordinate) will deallocate
 * other's page table and make it use dest's page table.  A lifetime
 * constraint is added whereby the dest cspd cannot be deallocated
 * before other.  This is done with the reference counting mechanism
 * already present.  Other could really be deallocated now without
 * worrying about the page-tables, except that references to it can
 * still be held by threads making invocations.  If these threads
 * could be pointed to dest instead of other, we could deallocate
 * other even earlier.  Perhaps version three of this will change the
 * ipc return path and if the cspd to return to is subordinate, return
 * to the subordinate's master instead, decrimenting the refcnt on the
 * subordinate.
 *
 * Assume that the composites passed in aren't the same.
 */
static struct composite_spd *mpd_merge(struct composite_spd *c1, 
				       struct composite_spd *c2)
{
	struct spd *curr;
	struct composite_spd *dest, *other;

	assert(NULL != c1 && NULL != c2);
	assert(spd_is_composite(&c1->spd_info) && spd_is_composite(&c2->spd_info));
	assert(!spd_mpd_is_depricated(c1) && !spd_mpd_is_depricated(c2));
	assert(!spd_mpd_is_subordinate(c1) && !spd_mpd_is_subordinate(c2));
	assert(c1 != c2);
	other = get_spd_to_subordinate(c1, c2);
	dest = (other == c1) ? c2 : c1;
	/*
	extern void print_valid_pgtbl_entries(phys_addr_t pt);
	print_valid_pgtbl_entries(dest->spd_info.pg_tbl);
	print_valid_pgtbl_entries(other->spd_info.pg_tbl);
	*/

	/* 
	 * While there are spds in the current composite, move them to
	 * the new composite.
	 */
	while (other->members) {
		curr = other->members;
		if (spd_composite_move_member(dest, curr)) {
			/* FIXME: should back out all those that were
			 * already transferred from one to the
			 * other...but this error is really only
			 * indicatory of an error in the kernel
			 * anyway. */
			printk("cos: could not move spd from one composite spd to another in the merge operation.\n");
			return NULL;
		}
		assert(other->members != curr);
	}
	//spd_mpd_depricate(other);
	spd_mpd_make_subordinate(dest, other);
	assert(!spd_mpd_is_depricated(dest) && !spd_mpd_is_subordinate(dest));
	//print_valid_pgtbl_entries(dest->spd_info.pg_tbl);

	return dest;
}

/* 
 * Here composite_spd and composite_dest are specified as normal spds,
 * and the meaning here is "the composite protection domain that this
 * spd is part of".
 */
COS_SYSCALL int cos_syscall_mpd_cntl(int spd_id, int operation, 
				     spdid_t spd1, spdid_t spd2)
{
	int ret = 0; 
	struct composite_spd *prev = NULL;
	struct spd *from;
	phys_addr_t curr_pg_tbl, new_pg_tbl;
	struct spd_poly *curr;

	from = spd_get_by_index(spd1);
	if (0 == from) {
		printk("cos: mpd_cntl -- first composite spd %d not valid\n", spd1);
		return -1;
	}
	prev = (struct composite_spd *)from->composite_spd;
	assert(prev && spd_is_composite(&prev->spd_info));
	assert(!spd_mpd_is_subordinate(prev) && !spd_mpd_is_depricated(prev));
	curr = thd_get_thd_spdpoly(thd_get_current());
	curr_pg_tbl = curr->pg_tbl;

	switch(operation) {
	case COS_MPD_SPLIT:
	{
		struct spd *transitory;
		struct composite_spd *trans_cspd;
		struct mpd_split_ret sret;

		transitory = spd_get_by_index(spd2);
		if (NULL == transitory) {
			printk("cos: mpd_cntl -- failed to access normal spd (%d) for call to split.\n", spd2);
			ret = -1;
			break;
		}
		trans_cspd = (struct composite_spd *)transitory->composite_spd;
		assert(spd_is_composite(&trans_cspd->spd_info));
		assert(!spd_mpd_is_depricated(trans_cspd) && !spd_mpd_is_subordinate(trans_cspd));
		assert(spd_composite_num_members(prev) > 0);
		if (trans_cspd != prev) {
			printk("cos: mpd_cntl -- spd %d not in claimed composite for %d\n", spd2, spd1);
			return -1;
		}
		/*
		 * It is not correct to split a spd out of a composite
		 * spd that only contains one spd.  It is pointless,
		 * and should not be encouraged.
		 */
		if (spd_composite_num_members(prev) == 1) {
			ret = -1;
			break;
		}
//		printk("cos: split spd with cspd %p from %p\n", trans_cspd, prev);
		ret = mpd_split(prev, transitory, &sret.new, &sret.old);
		/* simply return 0 for success */
/* 		if (!ret) { */
/* 			/\*  */
/* 			 * Pack the two short indexes of the mpds into */
/* 			 * a single int, and return that. */
/* 			 *\/ */
/* 			ret = *((int*)&sret); */
/* 		} */

		break;
	}
	case COS_MPD_MERGE:
	{
		struct spd *second;
		struct composite_spd *other, *cspd_ret;
		
		second = spd_get_by_index(spd2);
		if (0 == second) {
			printk("cos; mpd_cntl -- second composite spd %d invalid\n", spd2);
			ret = -1;
			break;
		}
		other = (struct composite_spd *)second->composite_spd;
		assert(spd_is_composite(&other->spd_info));
		assert(NULL != other && !spd_mpd_is_depricated(other) && !spd_mpd_is_subordinate(other));
//		printk("cos: merge %p and %p\n", prev, other);
		if (prev == other) {
//			printk("cos: skipping merge\n");
			ret = 0;
			break;
		}
		if (NULL == (cspd_ret = mpd_merge(prev, other))) {
			ret = -1;
			break;
		}
		ret = 0;
		//ret = spd_mpd_index(cspd_ret);
		break;
	}
	default:
		ret = -1;
	}

	new_pg_tbl = curr->pg_tbl;
	/*
	 * The page tables of the current spd can change if the
	 * current spd is subordinated to another spd.  If they did,
	 * we should do something about it:
	 */
	switch_pg_tbls(new_pg_tbl, curr_pg_tbl);
	
	return ret;
}

/*
 * Well look at that:  full support for mapping in 50 lines of code.
 *
 * FIXME: check that 1) spdid is allowed to map, and 2) check flags if
 * the spd wishes to confirm that the dspd is in the invocation stack,
 * or is in the current composite spd, or is a child of a fault
 * thread.
 */
extern int pgtbl_add_entry(phys_addr_t pgtbl, vaddr_t vaddr, phys_addr_t paddr); 
extern phys_addr_t pgtbl_rem_ret(phys_addr_t pgtbl, vaddr_t va);
COS_SYSCALL int cos_syscall_mmap_cntl(int spdid, long op_flags_dspd, vaddr_t daddr, long mem_id)
{
	short int op, flags, dspd_id;
	phys_addr_t page;
	int ret = 0;
	struct spd *spd;
	
	/* decode arguments */
	op = op_flags_dspd>>24;
	flags = op_flags_dspd>>16 & 0x000000FF;
	dspd_id = op_flags_dspd & 0x0000FFFF;

	spd = spd_get_by_index(dspd_id);
	if (NULL == spd || /*virtual_namespace_query(daddr) != spd*/
	    (daddr < spd->location.lowest_addr || 
	     daddr >= spd->location.lowest_addr + spd->location.size)) {
		//printk("cos: invalid mmap cntl call for spd %d for spd %d @ vaddr %x\n",
		//       spdid, dspd_id, (unsigned int)daddr);
		return -1;
	}

	switch(op) {
	case COS_MMAP_GRANT:
		page = cos_access_page(mem_id);
		if (0 == page) {
			printk("cos: mmap grant -- could not get a physical page.\n");
			ret = -1;
			break;
		}
		/*
		 * Demand paging could mess this up as the entry might
		 * not be in the page table, and we map in our cos
		 * page.  Ignore for the time being, as our loader
		 * forces demand paging to not be used (explicitely
		 * writing all of the pages itself).
		 */
		if (pgtbl_add_entry(spd->spd_info.pg_tbl, daddr, page)) {
			printk("cos: mmap grant -- could not add entry to page table.\n");
			ret = -1;
			break;
		}
		cos_meas_event(COS_MAP_GRANT);

		break;
	case COS_MMAP_REVOKE:
	{
		phys_addr_t pa;

		if (!(pa = pgtbl_rem_ret(spd->spd_info.pg_tbl, daddr))) {
			ret = 0;
			break;
		}
		ret = cos_phys_addr_to_cap(pa);
		cos_meas_event(COS_MAP_REVOKE);

		break;
	}
	default:
		ret = -1;
	}

	return ret;
}

COS_SYSCALL int cos_syscall_print(int spdid, char *str, int len)
{
	static char last = '\n';
	/*
	 * FIXME: use linux functions to copy the string into local
	 * storage to avoid faults.  ...This won't work with cos
	 * allocated memory, so we really just need to do a proper
	 * output system.  This is low prio as the string should be
	 * passed in the arg region.  Perhaps we should just check
	 * that.
	 */
	
	str[len] = '\0';
	if ('\n' == last)
		printk("cos,%d: %s", thd_get_id(thd_get_current()), str);
	else 
		printk("%s", str);
	last = str[len-1];
	return 0;
}

COS_SYSCALL int cos_syscall_cap_cntl(int spdid, spdid_t cspdid, spdid_t sspdid, int optional)
{
	struct spd *cspd, *sspd;
	cspd = spd_get_by_index(cspdid);
	sspd = spd_get_by_index(sspdid);

	if (!cspd || !sspd) return -1;
	/* TODO: access control */
	return spd_read_reset_invocation_cnt(cspd, sspd);
}

COS_SYSCALL int cos_syscall_stats(int spdid)
{
	cos_meas_report();
	cos_meas_init();
	printk("cos: brand w/ pending %ld\n", 
	       thd_get_by_id(15)->thread_brand->pending_upcall_requests);

	return 0;
}

/* 
 * Composite's system call table that is indexed and invoked by ipc.S.
 * The user-level stubs are created in cos_component.h.
 */
void *cos_syscall_tbl[16] = {
	(void*)cos_syscall_void,
	(void*)cos_syscall_stats,
	(void*)cos_syscall_print,
	(void*)cos_syscall_create_thread,
	(void*)cos_syscall_switch_thread,
	(void*)cos_syscall_kill_thd,
	(void*)cos_syscall_brand_upcall,
	(void*)cos_syscall_brand_cntl,
	(void*)cos_syscall_upcall,
	(void*)cos_syscall_sched_cntl,
	(void*)cos_syscall_mpd_cntl,
	(void*)cos_syscall_mmap_cntl,
	(void*)cos_syscall_brand_wire,
	(void*)cos_syscall_cap_cntl,
	(void*)cos_syscall_buff_mgmt,
	(void*)cos_syscall_thd_cntl
};
