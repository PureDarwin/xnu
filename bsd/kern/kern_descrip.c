/*
 * Copyright (c) 2000-2016 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/* Copyright (c) 1995, 1997 Apple Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_descrip.c	8.8 (Berkeley) 2/14/95
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2006 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/vnode_internal.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/file_internal.h>
#include <sys/guarded.h>
#include <sys/priv.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/fsctl.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syslog.h>
#include <sys/unistd.h>
#include <sys/resourcevar.h>
#include <sys/aio_kern.h>
#include <sys/ev.h>
#include <kern/locks.h>
#include <sys/uio_internal.h>
#include <sys/codesign.h>
#include <sys/codedir_internal.h>
#include <sys/mount_internal.h>
#include <sys/kdebug.h>
#include <sys/sysproto.h>
#include <sys/pipe.h>
#include <sys/spawn.h>
#include <sys/cprotect.h>
#include <sys/ubc_internal.h>

#include <kern/kern_types.h>
#include <kern/kalloc.h>
#include <kern/waitq.h>
#include <kern/ipc_kobject.h>
#include <kern/ipc_misc.h>
#include <kern/ast.h>

#include <vm/vm_protos.h>
#include <mach/mach_port.h>

#include <security/audit/audit.h>
#if CONFIG_MACF
#include <security/mac_framework.h>
#endif

#include <stdbool.h>
#include <os/atomic_private.h>
#include <os/overflow.h>
#include <IOKit/IOBSD.h>

void fileport_releasefg(struct fileglob *fg);

/* flags for fp_close_and_unlock */
#define FD_DUP2RESV 1

/* We don't want these exported */

__private_extern__
int unlink1(vfs_context_t, vnode_t, user_addr_t, enum uio_seg, int);

/* Conflict wait queue for when selects collide (opaque type) */
extern struct waitq select_conflict_queue;

#define f_flag fp_glob->fg_flag
#define f_type fp_glob->fg_ops->fo_type
#define f_cred fp_glob->fg_cred
#define f_ops fp_glob->fg_ops
#define f_offset fp_glob->fg_offset

ZONE_DEFINE_TYPE(fg_zone, "fileglob", struct fileglob, ZC_ZFREE_CLEARMEM);
ZONE_DEFINE_ID(ZONE_ID_FILEPROC, "fileproc", struct fileproc, ZC_ZFREE_CLEARMEM);

/*
 * Descriptor management.
 */
int nfiles;                     /* actual number of open files */
/*
 * "uninitialized" ops -- ensure FILEGLOB_DTYPE(fg) always exists
 */
static const struct fileops uninitops;

os_refgrp_decl(, f_refgrp, "files refcounts", NULL);
static LCK_GRP_DECLARE(file_lck_grp, "file");


#pragma mark fileglobs

/*!
 * @function fg_alloc_init
 *
 * @brief
 * Allocate and minimally initialize a file structure.
 */
struct fileglob *
fg_alloc_init(vfs_context_t ctx)
{
	struct fileglob *fg;

	fg = zalloc_flags(fg_zone, Z_WAITOK | Z_ZERO);
	lck_mtx_init(&fg->fg_lock, &file_lck_grp, LCK_ATTR_NULL);

	os_ref_init_raw(&fg->fg_count, &f_refgrp);
	fg->fg_ops = &uninitops;

	kauth_cred_ref(ctx->vc_ucred);
	fg->fg_cred = ctx->vc_ucred;

	os_atomic_inc(&nfiles, relaxed);

	return fg;
}

/*!
 * @function fg_free
 *
 * @brief
 * Free a file structure.
 */
static void
fg_free(struct fileglob *fg)
{
	os_atomic_dec(&nfiles, relaxed);

	if (fg->fg_vn_data) {
		fg_vn_data_free(fg->fg_vn_data);
		fg->fg_vn_data = NULL;
	}

	kauth_cred_t cred = fg->fg_cred;
	if (IS_VALID_CRED(cred)) {
		kauth_cred_unref(&cred);
		fg->fg_cred = NOCRED;
	}
	lck_mtx_destroy(&fg->fg_lock, &file_lck_grp);

#if CONFIG_MACF && CONFIG_VNGUARD
	vng_file_label_destroy(fg);
#endif
	zfree(fg_zone, fg);
}

OS_ALWAYS_INLINE
void
fg_ref(proc_t p, struct fileglob *fg)
{
#if DEBUG || DEVELOPMENT
	/* Allow fileglob refs to be taken outside of a process context. */
	if (p != FG_NOPROC) {
		proc_fdlock_assert(p, LCK_MTX_ASSERT_OWNED);
	}
#else
	(void)p;
#endif
	os_ref_retain_raw(&fg->fg_count, &f_refgrp);
}

void
fg_drop_live(struct fileglob *fg)
{
	os_ref_release_live_raw(&fg->fg_count, &f_refgrp);
}

int
fg_drop(proc_t p, struct fileglob *fg)
{
	struct vnode *vp;
	struct vfs_context context;
	int error = 0;

	if (fg == NULL) {
		return 0;
	}

	/* Set up context with cred stashed in fg */
	if (p == current_proc()) {
		context.vc_thread = current_thread();
	} else {
		context.vc_thread = NULL;
	}
	context.vc_ucred = fg->fg_cred;

	/*
	 * POSIX record locking dictates that any close releases ALL
	 * locks owned by this process.  This is handled by setting
	 * a flag in the unlock to free ONLY locks obeying POSIX
	 * semantics, and not to free BSD-style file locks.
	 * If the descriptor was in a message, POSIX-style locks
	 * aren't passed with the descriptor.
	 */
	if (p != FG_NOPROC && DTYPE_VNODE == FILEGLOB_DTYPE(fg) &&
	    (p->p_ladvflag & P_LADVLOCK)) {
		struct flock lf = {
			.l_whence = SEEK_SET,
			.l_type = F_UNLCK,
		};

		vp = (struct vnode *)fg_get_data(fg);
		if ((error = vnode_getwithref(vp)) == 0) {
			(void)VNOP_ADVLOCK(vp, (caddr_t)p, F_UNLCK, &lf, F_POSIX, &context, NULL);
			(void)vnode_put(vp);
		}
	}

	if (os_ref_release_raw(&fg->fg_count, &f_refgrp) == 0) {
		/*
		 * Since we ensure that fg->fg_ops is always initialized,
		 * it is safe to invoke fo_close on the fg
		 */
		error = fo_close(fg, &context);

		fg_free(fg);
	}

	return error;
}

inline
void
fg_set_data(
	struct fileglob *fg,
	void *fg_data)
{
	uintptr_t *store = &fg->fg_data;

#if __has_feature(ptrauth_calls)
	int type = FILEGLOB_DTYPE(fg);

	if (fg_data) {
		type ^= OS_PTRAUTH_DISCRIMINATOR("fileglob.fg_data");
		fg_data = ptrauth_sign_unauthenticated(fg_data,
		    ptrauth_key_process_independent_data,
		    ptrauth_blend_discriminator(store, type));
	}
#endif // __has_feature(ptrauth_calls)

	*store = (uintptr_t)fg_data;
}

inline
void *
fg_get_data_volatile(struct fileglob *fg)
{
	uintptr_t *store = &fg->fg_data;
	void *fg_data = (void *)*store;

#if __has_feature(ptrauth_calls)
	int type = FILEGLOB_DTYPE(fg);

	if (fg_data) {
		type ^= OS_PTRAUTH_DISCRIMINATOR("fileglob.fg_data");
		fg_data = ptrauth_auth_data(fg_data,
		    ptrauth_key_process_independent_data,
		    ptrauth_blend_discriminator(store, type));
	}
#endif // __has_feature(ptrauth_calls)

	return fg_data;
}

static void
fg_transfer_filelocks(proc_t p, struct fileglob *fg, thread_t thread)
{
	struct vnode *vp;
	struct vfs_context context;
	struct proc *old_proc = current_proc();

	assert(fg != NULL);

	assert(p != old_proc);
	context.vc_thread = thread;
	context.vc_ucred = fg->fg_cred;

	/* Transfer all POSIX Style locks to new proc */
	if (p && DTYPE_VNODE == FILEGLOB_DTYPE(fg) &&
	    (p->p_ladvflag & P_LADVLOCK)) {
		struct flock lf = {
			.l_whence = SEEK_SET,
			.l_start = 0,
			.l_len = 0,
			.l_type = F_TRANSFER,
		};

		vp = (struct vnode *)fg_get_data(fg);
		if (vnode_getwithref(vp) == 0) {
			(void)VNOP_ADVLOCK(vp, (caddr_t)old_proc, F_TRANSFER, &lf, F_POSIX, &context, NULL);
			(void)vnode_put(vp);
		}
	}

	/* Transfer all OFD Style locks to new proc */
	if (p && DTYPE_VNODE == FILEGLOB_DTYPE(fg) &&
	    (fg->fg_lflags & FG_HAS_OFDLOCK)) {
		struct flock lf = {
			.l_whence = SEEK_SET,
			.l_start = 0,
			.l_len = 0,
			.l_type = F_TRANSFER,
		};

		vp = (struct vnode *)fg_get_data(fg);
		if (vnode_getwithref(vp) == 0) {
			(void)VNOP_ADVLOCK(vp, ofd_to_id(fg), F_TRANSFER, &lf, F_OFD_LOCK, &context, NULL);
			(void)vnode_put(vp);
		}
	}
	return;
}

bool
fg_sendable(struct fileglob *fg)
{
	switch (FILEGLOB_DTYPE(fg)) {
	case DTYPE_VNODE:
	case DTYPE_SOCKET:
	case DTYPE_PIPE:
	case DTYPE_PSXSHM:
	case DTYPE_NETPOLICY:
		return (fg->fg_lflags & FG_CONFINED) == 0;

	default:
		return false;
	}
}

#pragma mark file descriptor table (static helpers)

static void
procfdtbl_reservefd(struct proc * p, int fd)
{
	p->p_fd.fd_ofiles[fd] = NULL;
	p->p_fd.fd_ofileflags[fd] |= UF_RESERVED;
}

void
procfdtbl_releasefd(struct proc * p, int fd, struct fileproc * fp)
{
	if (fp != NULL) {
		p->p_fd.fd_ofiles[fd] = fp;
	}
	p->p_fd.fd_ofileflags[fd] &= ~UF_RESERVED;
	if ((p->p_fd.fd_ofileflags[fd] & UF_RESVWAIT) == UF_RESVWAIT) {
		p->p_fd.fd_ofileflags[fd] &= ~UF_RESVWAIT;
		wakeup(&p->p_fd);
	}
}

static void
procfdtbl_waitfd(struct proc * p, int fd)
{
	p->p_fd.fd_ofileflags[fd] |= UF_RESVWAIT;
	msleep(&p->p_fd, &p->p_fd.fd_lock, PRIBIO, "ftbl_waitfd", NULL);
}

static void
procfdtbl_clearfd(struct proc * p, int fd)
{
	int waiting;

	waiting = (p->p_fd.fd_ofileflags[fd] & UF_RESVWAIT);
	p->p_fd.fd_ofiles[fd] = NULL;
	p->p_fd.fd_ofileflags[fd] = 0;
	if (waiting == UF_RESVWAIT) {
		wakeup(&p->p_fd);
	}
}

/*
 * fdrelse
 *
 * Description:	Inline utility function to free an fd in a filedesc
 *
 * Parameters:	fdp				Pointer to filedesc fd lies in
 *		fd				fd to free
 *		reserv				fd should be reserved
 *
 * Returns:	void
 *
 * Locks:	Assumes proc_fdlock for process pointing to fdp is held by
 *		the caller
 */
void
fdrelse(struct proc * p, int fd)
{
	struct filedesc *fdp = &p->p_fd;
	int nfd = 0;

	if (fd < fdp->fd_freefile) {
		fdp->fd_freefile = fd;
	}
#if DIAGNOSTIC
	if (fd >= fdp->fd_afterlast) {
		panic("fdrelse: fd_afterlast inconsistent");
	}
#endif
	procfdtbl_clearfd(p, fd);

	nfd = fdp->fd_afterlast;
	while (nfd > 0 && fdp->fd_ofiles[nfd - 1] == NULL &&
	    !(fdp->fd_ofileflags[nfd - 1] & UF_RESERVED)) {
		nfd--;
	}
	fdp->fd_afterlast = nfd;

#if CONFIG_PROC_RESOURCE_LIMITS
	fdp->fd_nfiles_open--;
#endif /* CONFIG_PROC_RESOURCE_LIMITS */
}


/*
 * finishdup
 *
 * Description:	Common code for dup, dup2, and fcntl(F_DUPFD).
 *
 * Parameters:	p				Process performing the dup
 *		old				The fd to dup
 *		new				The fd to dup it to
 *		fp_flags			Flags to augment the new fp
 *		retval				Pointer to the call return area
 *
 * Returns:	0				Success
 *		EBADF
 *		ENOMEM
 *
 * Implicit returns:
 *		*retval (modified)		The new descriptor
 *
 * Locks:	Assumes proc_fdlock for process pointing to fdp is held by
 *		the caller
 *
 * Notes:	This function may drop and reacquire this lock; it is unsafe
 *		for a caller to assume that other state protected by the lock
 *		has not been subsequently changed out from under it.
 */
static int
finishdup(
	proc_t                  p,
	kauth_cred_t            p_cred,
	int                     old,
	int                     new,
	fileproc_flags_t        fp_flags,
	int32_t                *retval)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *nfp;
	struct fileproc *ofp;
#if CONFIG_MACF
	int error;
#endif

#if DIAGNOSTIC
	proc_fdlock_assert(p, LCK_MTX_ASSERT_OWNED);
#endif
	if ((ofp = fdp->fd_ofiles[old]) == NULL ||
	    (fdp->fd_ofileflags[old] & UF_RESERVED)) {
		fdrelse(p, new);
		return EBADF;
	}

#if CONFIG_MACF
	error = mac_file_check_dup(p_cred, ofp->fp_glob, new);

	if (error) {
		fdrelse(p, new);
		return error;
	}
#else
	(void)p_cred;
#endif

	fg_ref(p, ofp->fp_glob);

	proc_fdunlock(p);

	nfp = fileproc_alloc_init();

	if (fp_flags) {
		nfp->fp_flags |= fp_flags;
	}
	nfp->fp_glob = ofp->fp_glob;

	proc_fdlock(p);

#if DIAGNOSTIC
	if (fdp->fd_ofiles[new] != 0) {
		panic("finishdup: overwriting fd_ofiles with new %d", new);
	}
	if ((fdp->fd_ofileflags[new] & UF_RESERVED) == 0) {
		panic("finishdup: unreserved fileflags with new %d", new);
	}
#endif

	if (new >= fdp->fd_afterlast) {
		fdp->fd_afterlast = new + 1;
	}
	procfdtbl_releasefd(p, new, nfp);
	*retval = new;
	return 0;
}


#pragma mark file descriptor table (exported functions)

void
proc_dirs_lock_shared(proc_t p)
{
	lck_rw_lock_shared(&p->p_fd.fd_dirs_lock);
}

void
proc_dirs_unlock_shared(proc_t p)
{
	lck_rw_unlock_shared(&p->p_fd.fd_dirs_lock);
}

void
proc_dirs_lock_exclusive(proc_t p)
{
	lck_rw_lock_exclusive(&p->p_fd.fd_dirs_lock);
}

void
proc_dirs_unlock_exclusive(proc_t p)
{
	lck_rw_unlock_exclusive(&p->p_fd.fd_dirs_lock);
}

/*
 * proc_fdlock, proc_fdlock_spin
 *
 * Description:	Lock to control access to the per process struct fileproc
 *		and struct filedesc
 *
 * Parameters:	p				Process to take the lock on
 *
 * Returns:	void
 *
 * Notes:	The lock is initialized in forkproc() and destroyed in
 *		reap_child_process().
 */
void
proc_fdlock(proc_t p)
{
	lck_mtx_lock(&p->p_fd.fd_lock);
}

void
proc_fdlock_spin(proc_t p)
{
	lck_mtx_lock_spin(&p->p_fd.fd_lock);
}

void
proc_fdlock_assert(proc_t p, int assertflags)
{
	lck_mtx_assert(&p->p_fd.fd_lock, assertflags);
}


/*
 * proc_fdunlock
 *
 * Description:	Unlock the lock previously locked by a call to proc_fdlock()
 *
 * Parameters:	p				Process to drop the lock on
 *
 * Returns:	void
 */
void
proc_fdunlock(proc_t p)
{
	lck_mtx_unlock(&p->p_fd.fd_lock);
}

bool
fdt_available_locked(proc_t p, int n)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc **fpp;
	char *flags;
	int i;
	int lim = proc_limitgetcur_nofile(p);

	if ((i = lim - fdp->fd_nfiles) > 0 && (n -= i) <= 0) {
		return true;
	}
	fpp = &fdp->fd_ofiles[fdp->fd_freefile];
	flags = &fdp->fd_ofileflags[fdp->fd_freefile];
	for (i = fdp->fd_nfiles - fdp->fd_freefile; --i >= 0; fpp++, flags++) {
		if (*fpp == NULL && !(*flags & UF_RESERVED) && --n <= 0) {
			return true;
		}
	}
	return false;
}


struct fdt_iterator
fdt_next(proc_t p, int fd, bool only_settled)
{
	struct fdt_iterator it;
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp;
	int nfds = fdp->fd_afterlast;

	while (++fd < nfds) {
		fp = fdp->fd_ofiles[fd];
		if (fp == NULL || fp->fp_glob == NULL) {
			continue;
		}
		if (only_settled && (fdp->fd_ofileflags[fd] & UF_RESERVED)) {
			continue;
		}
		it.fdti_fd = fd;
		it.fdti_fp = fp;
		return it;
	}

	it.fdti_fd = nfds;
	it.fdti_fp = NULL;
	return it;
}

struct fdt_iterator
fdt_prev(proc_t p, int fd, bool only_settled)
{
	struct fdt_iterator it;
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp;

	while (--fd >= 0) {
		fp = fdp->fd_ofiles[fd];
		if (fp == NULL || fp->fp_glob == NULL) {
			continue;
		}
		if (only_settled && (fdp->fd_ofileflags[fd] & UF_RESERVED)) {
			continue;
		}
		it.fdti_fd = fd;
		it.fdti_fp = fp;
		return it;
	}

	it.fdti_fd = -1;
	it.fdti_fp = NULL;
	return it;
}

void
fdt_init(proc_t p)
{
	struct filedesc *fdp = &p->p_fd;

	lck_mtx_init(&fdp->fd_kqhashlock, &proc_kqhashlock_grp, &proc_lck_attr);
	lck_mtx_init(&fdp->fd_knhashlock, &proc_knhashlock_grp, &proc_lck_attr);
	lck_mtx_init(&fdp->fd_lock, &proc_fdmlock_grp, &proc_lck_attr);
	lck_rw_init(&fdp->fd_dirs_lock, &proc_dirslock_grp, &proc_lck_attr);
}

void
fdt_destroy(proc_t p)
{
	struct filedesc *fdp = &p->p_fd;

	lck_mtx_destroy(&fdp->fd_kqhashlock, &proc_kqhashlock_grp);
	lck_mtx_destroy(&fdp->fd_knhashlock, &proc_knhashlock_grp);
	lck_mtx_destroy(&fdp->fd_lock, &proc_fdmlock_grp);
	lck_rw_destroy(&fdp->fd_dirs_lock, &proc_dirslock_grp);
}

void
fdt_exec(proc_t p, kauth_cred_t p_cred, short posix_spawn_flags, thread_t thread, bool in_exec)
{
	struct filedesc *fdp = &p->p_fd;
	thread_t self = current_thread();
	struct uthread *ut = get_bsdthread_info(self);
	struct kqworkq *dealloc_kqwq = NULL;

	/*
	 * If the current thread is bound as a workq/workloop
	 * servicing thread, we need to unbind it first.
	 */
	if (ut->uu_kqr_bound && get_bsdthreadtask_info(self) == p) {
		kqueue_threadreq_unbind(p, ut->uu_kqr_bound);
	}

	/*
	 * Deallocate the knotes for this process
	 * and mark the tables non-existent so
	 * subsequent kqueue closes go faster.
	 */
	knotes_dealloc(p);
	assert(fdp->fd_knlistsize == 0);
	assert(fdp->fd_knhashmask == 0);

	proc_fdlock(p);

	/* Set the P_LADVLOCK flag if the flag set on old proc */
	if (in_exec && (current_proc()->p_ladvflag & P_LADVLOCK)) {
		os_atomic_or(&p->p_ladvflag, P_LADVLOCK, relaxed);
	}

	for (int i = fdp->fd_afterlast; i-- > 0;) {
		struct fileproc *fp = fdp->fd_ofiles[i];
		char *flagp = &fdp->fd_ofileflags[i];
		bool inherit_file = true;

		if (fp == FILEPROC_NULL) {
			continue;
		}

		/*
		 * no file descriptor should be in flux when in exec,
		 * because we stopped all other threads
		 */
		if (*flagp & ~UF_INHERIT) {
			panic("file %d/%p in flux during exec of %p", i, fp, p);
		}

		if (fp->fp_flags & FP_CLOEXEC) {
			inherit_file = false;
		} else if ((posix_spawn_flags & POSIX_SPAWN_CLOEXEC_DEFAULT) &&
		    !(*flagp & UF_INHERIT)) {
			/*
			 * Reverse the usual semantics of file descriptor
			 * inheritance - all of them should be closed
			 * except files marked explicitly as "inherit" and
			 * not marked close-on-exec.
			 */
			inherit_file = false;
#if CONFIG_MACF
		} else if (mac_file_check_inherit(p_cred, fp->fp_glob)) {
			inherit_file = false;
#endif
		}

		*flagp = 0; /* clear UF_INHERIT */

		if (!inherit_file) {
			fp_close_and_unlock(p, p_cred, i, fp, 0);
			proc_fdlock(p);
		} else if (in_exec) {
			/* Transfer F_POSIX style lock to new proc */
			proc_fdunlock(p);
			fg_transfer_filelocks(p, fp->fp_glob, thread);
			proc_fdlock(p);
		}
	}

	/* release the per-process workq kq */
	if (fdp->fd_wqkqueue) {
		dealloc_kqwq = fdp->fd_wqkqueue;
		fdp->fd_wqkqueue = NULL;
	}

	proc_fdunlock(p);

	/* Anything to free? */
	if (dealloc_kqwq) {
		kqworkq_dealloc(dealloc_kqwq);
	}
}


int
fdt_fork(struct filedesc *newfdp, proc_t p, vnode_t uth_cdir, bool in_exec)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc **ofiles;
	char *ofileflags;
	int n_files, afterlast, freefile;
	vnode_t v_dir;
#if CONFIG_PROC_RESOURCE_LIMITS
	int fd_nfiles_open = 0;
#endif /* CONFIG_PROC_RESOURCE_LIMITS */
	proc_fdlock(p);

	newfdp->fd_flags = (fdp->fd_flags & FILEDESC_FORK_INHERITED_MASK);
	newfdp->fd_cmask = fdp->fd_cmask;
#if CONFIG_PROC_RESOURCE_LIMITS
	newfdp->fd_nfiles_soft_limit = fdp->fd_nfiles_soft_limit;
	newfdp->fd_nfiles_hard_limit = fdp->fd_nfiles_hard_limit;

	newfdp->kqwl_dyn_soft_limit = fdp->kqwl_dyn_soft_limit;
	newfdp->kqwl_dyn_hard_limit = fdp->kqwl_dyn_hard_limit;
#endif /* CONFIG_PROC_RESOURCE_LIMITS */

	/*
	 * For both fd_cdir and fd_rdir make sure we get
	 * a valid reference... if we can't, than set
	 * set the pointer(s) to NULL in the child... this
	 * will keep us from using a non-referenced vp
	 * and allows us to do the vnode_rele only on
	 * a properly referenced vp
	 */
	if ((v_dir = fdp->fd_rdir)) {
		if (vnode_getwithref(v_dir) == 0) {
			if (vnode_ref(v_dir) == 0) {
				newfdp->fd_rdir = v_dir;
			}
			vnode_put(v_dir);
		}
		if (newfdp->fd_rdir == NULL) {
			/*
			 * We couldn't get a new reference on
			 * the chroot directory being
			 * inherited... this is fatal, since
			 * otherwise it would constitute an
			 * escape from a chroot environment by
			 * the new process.
			 */
			proc_fdunlock(p);
			return EPERM;
		}
	}

	/*
	 * If we are running with per-thread current working directories,
	 * inherit the new current working directory from the current thread.
	 */
	if ((v_dir = uth_cdir ? uth_cdir : fdp->fd_cdir)) {
		if (vnode_getwithref(v_dir) == 0) {
			if (vnode_ref(v_dir) == 0) {
				newfdp->fd_cdir = v_dir;
			}
			vnode_put(v_dir);
		}
		if (newfdp->fd_cdir == NULL && v_dir == fdp->fd_cdir) {
			/*
			 * we couldn't get a new reference on
			 * the current working directory being
			 * inherited... we might as well drop
			 * our reference from the parent also
			 * since the vnode has gone DEAD making
			 * it useless... by dropping it we'll
			 * be that much closer to recycling it
			 */
			vnode_rele(fdp->fd_cdir);
			fdp->fd_cdir = NULL;
		}
	}

	/*
	 * If the number of open files fits in the internal arrays
	 * of the open file structure, use them, otherwise allocate
	 * additional memory for the number of descriptors currently
	 * in use.
	 */
	afterlast = fdp->fd_afterlast;
	freefile = fdp->fd_freefile;
	if (afterlast <= NDFILE) {
		n_files = NDFILE;
	} else {
		n_files = roundup(afterlast, NDEXTENT);
	}

	proc_fdunlock(p);

	ofiles = kalloc_type(struct fileproc *, n_files, Z_WAITOK | Z_ZERO);
	ofileflags = kalloc_data(n_files, Z_WAITOK | Z_ZERO);
	if (ofiles == NULL || ofileflags == NULL) {
		kfree_type(struct fileproc *, n_files, ofiles);
		kfree_data(ofileflags, n_files);
		if (newfdp->fd_cdir) {
			vnode_rele(newfdp->fd_cdir);
			newfdp->fd_cdir = NULL;
		}
		if (newfdp->fd_rdir) {
			vnode_rele(newfdp->fd_rdir);
			newfdp->fd_rdir = NULL;
		}
		return ENOMEM;
	}

	proc_fdlock(p);

	for (int i = afterlast; i-- > 0;) {
		struct fileproc *ofp, *nfp;
		char flags;

		ofp = fdp->fd_ofiles[i];
		flags = fdp->fd_ofileflags[i];

		if (ofp == NULL ||
		    (ofp->fp_glob->fg_lflags & FG_CONFINED) ||
		    ((ofp->fp_flags & FP_CLOFORK) && !in_exec) ||
		    ((ofp->fp_flags & FP_CLOEXEC) && in_exec) ||
		    (flags & UF_RESERVED)) {
			if (i + 1 == afterlast) {
				afterlast = i;
			}
			if (i < freefile) {
				freefile = i;
			}

			continue;
		}

		nfp = fileproc_alloc_init();
		nfp->fp_glob = ofp->fp_glob;
		if (in_exec) {
			nfp->fp_flags = (ofp->fp_flags & (FP_CLOEXEC | FP_CLOFORK));
			if (ofp->fp_guard_attrs) {
				guarded_fileproc_copy_guard(ofp, nfp);
			}
		} else {
			assert(ofp->fp_guard_attrs == 0);
			nfp->fp_flags = (ofp->fp_flags & FP_CLOEXEC);
		}
		fg_ref(p, nfp->fp_glob);

		ofiles[i] = nfp;
#if CONFIG_PROC_RESOURCE_LIMITS
		fd_nfiles_open++;
#endif /* CONFIG_PROC_RESOURCE_LIMITS */
	}

	proc_fdunlock(p);

	newfdp->fd_ofiles = ofiles;
	newfdp->fd_ofileflags = ofileflags;
	newfdp->fd_nfiles = n_files;
	newfdp->fd_afterlast = afterlast;
	newfdp->fd_freefile = freefile;

#if CONFIG_PROC_RESOURCE_LIMITS
	newfdp->fd_nfiles_open = fd_nfiles_open;
#endif /* CONFIG_PROC_RESOURCE_LIMITS */

	return 0;
}

void
fdt_invalidate(proc_t p)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp, **ofiles;
	kauth_cred_t p_cred;
	char *ofileflags;
	struct kqworkq *kqwq = NULL;
	vnode_t vn1 = NULL, vn2 = NULL;
	struct kqwllist *kqhash = NULL;
	u_long kqhashmask = 0;
	int n_files = 0;

	/*
	 * deallocate all the knotes up front and claim empty
	 * tables to make any subsequent kqueue closes faster.
	 */
	knotes_dealloc(p);
	assert(fdp->fd_knlistsize == 0);
	assert(fdp->fd_knhashmask == 0);

	/*
	 * dealloc all workloops that have outstanding retains
	 * when created with scheduling parameters.
	 */
	kqworkloops_dealloc(p);

	proc_fdlock(p);

	/* proc_ucred_unsafe() is ok: process is terminating */
	p_cred = proc_ucred_unsafe(p);

	/* close file descriptors */
	if (fdp->fd_nfiles > 0 && fdp->fd_ofiles) {
		for (int i = fdp->fd_afterlast; i-- > 0;) {
			if ((fp = fdp->fd_ofiles[i]) != NULL) {
				if (fdp->fd_ofileflags[i] & UF_RESERVED) {
					panic("fdfree: found fp with UF_RESERVED");
				}
				/* proc_ucred_unsafe() is ok: process is terminating */
				fp_close_and_unlock(p, p_cred, i, fp, 0);
				proc_fdlock(p);
			}
		}
	}

	n_files = fdp->fd_nfiles;
	ofileflags = fdp->fd_ofileflags;
	ofiles = fdp->fd_ofiles;
	kqwq = fdp->fd_wqkqueue;
	vn1 = fdp->fd_cdir;
	vn2 = fdp->fd_rdir;

	fdp->fd_ofileflags = NULL;
	fdp->fd_ofiles = NULL;
	fdp->fd_nfiles = 0;
	fdp->fd_wqkqueue = NULL;
	fdp->fd_cdir = NULL;
	fdp->fd_rdir = NULL;

	proc_fdunlock(p);

	lck_mtx_lock(&fdp->fd_kqhashlock);

	kqhash = fdp->fd_kqhash;
	kqhashmask = fdp->fd_kqhashmask;

	fdp->fd_kqhash = 0;
	fdp->fd_kqhashmask = 0;

	lck_mtx_unlock(&fdp->fd_kqhashlock);

	kfree_type(struct fileproc *, n_files, ofiles);
	kfree_data(ofileflags, n_files);

	if (kqwq) {
		kqworkq_dealloc(kqwq);
	}
	if (vn1) {
		vnode_rele(vn1);
	}
	if (vn2) {
		vnode_rele(vn2);
	}
	if (kqhash) {
		for (uint32_t i = 0; i <= kqhashmask; i++) {
			assert(LIST_EMPTY(&kqhash[i]));
		}
		hashdestroy(kqhash, M_KQUEUE, kqhashmask);
	}
}


struct fileproc *
fileproc_alloc_init(void)
{
	struct fileproc *fp;

	fp = zalloc_id(ZONE_ID_FILEPROC, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	os_ref_init(&fp->fp_iocount, &f_refgrp);
	return fp;
}


void
fileproc_free(struct fileproc *fp)
{
	os_ref_release_last(&fp->fp_iocount);
	if (fp->fp_guard_attrs) {
		guarded_fileproc_unguard(fp);
	}
	assert(fp->fp_wset == NULL);
	zfree_id(ZONE_ID_FILEPROC, fp);
}


/*
 * Statistics counter for the number of times a process calling fdalloc()
 * has resulted in an expansion of the per process open file table.
 *
 * XXX This would likely be of more use if it were per process
 */
int fdexpand;

#if CONFIG_PROC_RESOURCE_LIMITS
/*
 * Should be called only with the proc_fdlock held.
 */
void
fd_check_limit_exceeded(struct filedesc *fdp)
{
#if DIAGNOSTIC
	proc_fdlock_assert(p, LCK_MTX_ASSERT_OWNED);
#endif

	if (!fd_above_soft_limit_notified(fdp) && fdp->fd_nfiles_soft_limit &&
	    (fdp->fd_nfiles_open > fdp->fd_nfiles_soft_limit)) {
		fd_above_soft_limit_send_notification(fdp);
		act_set_astproc_resource(current_thread());
	} else if (!fd_above_hard_limit_notified(fdp) && fdp->fd_nfiles_hard_limit &&
	    (fdp->fd_nfiles_open > fdp->fd_nfiles_hard_limit)) {
		fd_above_hard_limit_send_notification(fdp);
		act_set_astproc_resource(current_thread());
	}
}
#endif /* CONFIG_PROC_RESOURCE_LIMITS */

/*
 * fdalloc
 *
 * Description:	Allocate a file descriptor for the process.
 *
 * Parameters:	p				Process to allocate the fd in
 *		want				The fd we would prefer to get
 *		result				Pointer to fd we got
 *
 * Returns:	0				Success
 *		EMFILE
 *		ENOMEM
 *
 * Implicit returns:
 *		*result (modified)		The fd which was allocated
 */
int
fdalloc(proc_t p, int want, int *result)
{
	struct filedesc *fdp = &p->p_fd;
	int i;
	int last, numfiles, oldnfiles;
	struct fileproc **newofiles;
	char *newofileflags;
	int lim = proc_limitgetcur_nofile(p);

	/*
	 * Search for a free descriptor starting at the higher
	 * of want or fd_freefile.  If that fails, consider
	 * expanding the ofile array.
	 */
#if DIAGNOSTIC
	proc_fdlock_assert(p, LCK_MTX_ASSERT_OWNED);
#endif

	for (;;) {
		last = (int)MIN((unsigned int)fdp->fd_nfiles, (unsigned int)lim);
		if ((i = want) < fdp->fd_freefile) {
			i = fdp->fd_freefile;
		}
		for (; i < last; i++) {
			if (fdp->fd_ofiles[i] == NULL && !(fdp->fd_ofileflags[i] & UF_RESERVED)) {
				procfdtbl_reservefd(p, i);
				if (i >= fdp->fd_afterlast) {
					fdp->fd_afterlast = i + 1;
				}
				if (want <= fdp->fd_freefile) {
					fdp->fd_freefile = i;
				}
				*result = i;
#if CONFIG_PROC_RESOURCE_LIMITS
				fdp->fd_nfiles_open++;
				fd_check_limit_exceeded(fdp);
#endif /* CONFIG_PROC_RESOURCE_LIMITS */
				return 0;
			}
		}

		/*
		 * No space in current array.  Expand?
		 */
		if ((rlim_t)fdp->fd_nfiles >= lim) {
			return EMFILE;
		}
		if (fdp->fd_nfiles < NDEXTENT) {
			numfiles = NDEXTENT;
		} else {
			numfiles = 2 * fdp->fd_nfiles;
		}
		/* Enforce lim */
		if ((rlim_t)numfiles > lim) {
			numfiles = (int)lim;
		}
		proc_fdunlock(p);
		newofiles = kalloc_type(struct fileproc *, numfiles, Z_WAITOK | Z_ZERO);
		newofileflags = kalloc_data(numfiles, Z_WAITOK | Z_ZERO);
		proc_fdlock(p);
		if (newofileflags == NULL || newofiles == NULL) {
			kfree_type(struct fileproc *, numfiles, newofiles);
			kfree_data(newofileflags, numfiles);
			return ENOMEM;
		}
		if (fdp->fd_nfiles >= numfiles) {
			kfree_type(struct fileproc *, numfiles, newofiles);
			kfree_data(newofileflags, numfiles);
			continue;
		}

		/*
		 * Copy the existing ofile and ofileflags arrays
		 * and zero the new portion of each array.
		 */
		oldnfiles = fdp->fd_nfiles;
		memcpy(newofiles, fdp->fd_ofiles,
		    oldnfiles * sizeof(*fdp->fd_ofiles));
		memcpy(newofileflags, fdp->fd_ofileflags, oldnfiles);

		kfree_type(struct fileproc *, oldnfiles, fdp->fd_ofiles);
		kfree_data(fdp->fd_ofileflags, oldnfiles);
		fdp->fd_ofiles = newofiles;
		fdp->fd_ofileflags = newofileflags;
		fdp->fd_nfiles = numfiles;
		fdexpand++;
	}
}


#pragma mark fileprocs

void
fileproc_modify_vflags(struct fileproc *fp, fileproc_vflags_t vflags, boolean_t clearflags)
{
	if (clearflags) {
		os_atomic_andnot(&fp->fp_vflags, vflags, relaxed);
	} else {
		os_atomic_or(&fp->fp_vflags, vflags, relaxed);
	}
}

fileproc_vflags_t
fileproc_get_vflags(struct fileproc *fp)
{
	return os_atomic_load(&fp->fp_vflags, relaxed);
}

/*
 * falloc_withinit
 *
 * Create a new open file structure and allocate
 * a file descriptor for the process that refers to it.
 *
 * Returns:	0			Success
 *
 * Description:	Allocate an entry in the per process open file table and
 *		return the corresponding fileproc and fd.
 *
 * Parameters:	p				The process in whose open file
 *						table the fd is to be allocated
 *		resultfp			Pointer to fileproc pointer
 *						return area
 *		resultfd			Pointer to fd return area
 *		ctx				VFS context
 *		fp_zalloc			fileproc allocator to use
 *		crarg				allocator args
 *
 * Returns:	0				Success
 *		ENFILE				Too many open files in system
 *		fdalloc:EMFILE			Too many open files in process
 *		fdalloc:ENOMEM			M_OFILETABL zone exhausted
 *		ENOMEM				fp_zone or fg_zone zone
 *						exhausted
 *
 * Implicit returns:
 *		*resultfd (modified)		Returned fileproc pointer
 *		*resultfd (modified)		Returned fd
 *
 * Notes:	This function takes separate process and context arguments
 *		solely to support kern_exec.c; otherwise, it would take
 *		neither, and use the vfs_context_current() routine internally.
 */
int
falloc_withinit(
	proc_t                  p,
	struct ucred           *p_cred,
	struct vfs_context     *ctx,
	struct fileproc       **resultfp,
	int                    *resultfd,
	fp_initfn_t             fp_init,
	void                   *initarg)
{
	struct fileproc *fp;
	struct fileglob *fg;
	int error, nfd;

	/* Make sure we don't go beyond the system-wide limit */
	if (nfiles >= maxfiles) {
		tablefull("file");
		return ENFILE;
	}

	proc_fdlock(p);

	/* fdalloc will make sure the process stays below per-process limit */
	if ((error = fdalloc(p, 0, &nfd))) {
		proc_fdunlock(p);
		return error;
	}

#if CONFIG_MACF
	error = mac_file_check_create(p_cred);
	if (error) {
		proc_fdunlock(p);
		return error;
	}
#else
	(void)p_cred;
#endif

	/*
	 * Allocate a new file descriptor.
	 * If the process has file descriptor zero open, add to the list
	 * of open files at that point, otherwise put it at the front of
	 * the list of open files.
	 */
	proc_fdunlock(p);

	fp = fileproc_alloc_init();
	if (fp_init) {
		fp_init(fp, initarg);
	}

	fg = fg_alloc_init(ctx);

	os_ref_retain_locked(&fp->fp_iocount);
	fp->fp_glob = fg;

	proc_fdlock(p);

	p->p_fd.fd_ofiles[nfd] = fp;

	proc_fdunlock(p);

	if (resultfp) {
		*resultfp = fp;
	}
	if (resultfd) {
		*resultfd = nfd;
	}

	return 0;
}

/*
 * fp_free
 *
 * Description:	Release the fd and free the fileproc associated with the fd
 *		in the per process open file table of the specified process;
 *		these values must correspond.
 *
 * Parameters:	p				Process containing fd
 *		fd				fd to be released
 *		fp				fileproc to be freed
 */
void
fp_free(proc_t p, int fd, struct fileproc * fp)
{
	proc_fdlock_spin(p);
	fdrelse(p, fd);
	proc_fdunlock(p);

	fg_free(fp->fp_glob);
	os_ref_release_live(&fp->fp_iocount);
	fileproc_free(fp);
}


struct fileproc *
fp_get_noref_locked(proc_t p, int fd)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp;

	if (fd < 0 || fd >= fdp->fd_nfiles ||
	    (fp = fdp->fd_ofiles[fd]) == NULL ||
	    (fdp->fd_ofileflags[fd] & UF_RESERVED)) {
		return NULL;
	}

	zone_id_require(ZONE_ID_FILEPROC, sizeof(*fp), fp);
	return fp;
}

struct fileproc *
fp_get_noref_locked_with_iocount(proc_t p, int fd)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp = NULL;

	if (fd < 0 || fd >= fdp->fd_nfiles ||
	    (fp = fdp->fd_ofiles[fd]) == NULL ||
	    os_ref_get_count(&fp->fp_iocount) <= 1 ||
	    ((fdp->fd_ofileflags[fd] & UF_RESERVED) &&
	    !(fdp->fd_ofileflags[fd] & UF_CLOSING))) {
		panic("%s: caller without an ioccount on fileproc (%d/:%p)",
		    __func__, fd, fp);
	}

	zone_id_require(ZONE_ID_FILEPROC, sizeof(*fp), fp);
	return fp;
}


/*
 * fp_lookup
 *
 * Description:	Get fileproc pointer for a given fd from the per process
 *		open file table of the specified process and if successful,
 *		increment the fp_iocount
 *
 * Parameters:	p				Process in which fd lives
 *		fd				fd to get information for
 *		resultfp			Pointer to result fileproc
 *						pointer area, or 0 if none
 *		locked				!0 if the caller holds the
 *						proc_fdlock, 0 otherwise
 *
 * Returns:	0			Success
 *		EBADF			Bad file descriptor
 *
 * Implicit returns:
 *		*resultfp (modified)		Fileproc pointer
 *
 * Locks:	If the argument 'locked' is non-zero, then the caller is
 *		expected to have taken and held the proc_fdlock; if it is
 *		zero, than this routine internally takes and drops this lock.
 */
int
fp_lookup(proc_t p, int fd, struct fileproc **resultfp, int locked)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp;

	if (!locked) {
		proc_fdlock_spin(p);
	}
	if (fd < 0 || fdp == NULL || fd >= fdp->fd_nfiles ||
	    (fp = fdp->fd_ofiles[fd]) == NULL ||
	    (fdp->fd_ofileflags[fd] & UF_RESERVED)) {
		if (!locked) {
			proc_fdunlock(p);
		}
		return EBADF;
	}

	zone_id_require(ZONE_ID_FILEPROC, sizeof(*fp), fp);
	os_ref_retain_locked(&fp->fp_iocount);

	if (resultfp) {
		*resultfp = fp;
	}
	if (!locked) {
		proc_fdunlock(p);
	}

	return 0;
}


int
fp_get_ftype(proc_t p, int fd, file_type_t ftype, int err, struct fileproc **fpp)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp;

	proc_fdlock_spin(p);
	if (fd < 0 || fd >= fdp->fd_nfiles ||
	    (fp = fdp->fd_ofiles[fd]) == NULL ||
	    (fdp->fd_ofileflags[fd] & UF_RESERVED)) {
		proc_fdunlock(p);
		return EBADF;
	}

	if (fp->f_type != ftype) {
		proc_fdunlock(p);
		return err;
	}

	zone_id_require(ZONE_ID_FILEPROC, sizeof(*fp), fp);
	os_ref_retain_locked(&fp->fp_iocount);
	proc_fdunlock(p);

	*fpp = fp;
	return 0;
}


/*
 * fp_drop
 *
 * Description:	Drop the I/O reference previously taken by calling fp_lookup
 *		et. al.
 *
 * Parameters:	p				Process in which the fd lives
 *		fd				fd associated with the fileproc
 *		fp				fileproc on which to set the
 *						flag and drop the reference
 *		locked				flag to internally take and
 *						drop proc_fdlock if it is not
 *						already held by the caller
 *
 * Returns:	0				Success
 *		EBADF				Bad file descriptor
 *
 * Locks:	This function internally takes and drops the proc_fdlock for
 *		the supplied process if 'locked' is non-zero, and assumes that
 *		the caller already holds this lock if 'locked' is non-zero.
 *
 * Notes:	The fileproc must correspond to the fd in the supplied proc
 */
int
fp_drop(proc_t p, int fd, struct fileproc *fp, int locked)
{
	struct filedesc *fdp = &p->p_fd;
	int     needwakeup = 0;

	if (!locked) {
		proc_fdlock_spin(p);
	}
	if ((fp == FILEPROC_NULL) && (fd < 0 || fd >= fdp->fd_nfiles ||
	    (fp = fdp->fd_ofiles[fd]) == NULL ||
	    ((fdp->fd_ofileflags[fd] & UF_RESERVED) &&
	    !(fdp->fd_ofileflags[fd] & UF_CLOSING)))) {
		if (!locked) {
			proc_fdunlock(p);
		}
		return EBADF;
	}

	if (1 == os_ref_release_locked(&fp->fp_iocount)) {
		if (fp->fp_flags & FP_SELCONFLICT) {
			fp->fp_flags &= ~FP_SELCONFLICT;
		}

		if (fdp->fd_fpdrainwait) {
			fdp->fd_fpdrainwait = 0;
			needwakeup = 1;
		}
	}
	if (!locked) {
		proc_fdunlock(p);
	}
	if (needwakeup) {
		wakeup(&fdp->fd_fpdrainwait);
	}

	return 0;
}


/*
 * fileproc_drain
 *
 * Description:	Drain out pending I/O operations
 *
 * Parameters:	p				Process closing this file
 *		fp				fileproc struct for the open
 *						instance on the file
 *
 * Returns:	void
 *
 * Locks:	Assumes the caller holds the proc_fdlock
 *
 * Notes:	For character devices, this occurs on the last close of the
 *		device; for all other file descriptors, this occurs on each
 *		close to prevent fd's from being closed out from under
 *		operations currently in progress and blocked
 *
 * See Also:    file_vnode(), file_socket(), file_drop(), and the cautions
 *		regarding their use and interaction with this function.
 */
static void
fileproc_drain(proc_t p, struct fileproc * fp)
{
	struct filedesc *fdp = &p->p_fd;
	struct vfs_context context;
	thread_t thread;
	bool is_current_proc;

	is_current_proc = (p == current_proc());

	if (!is_current_proc) {
		proc_lock(p);
		thread = proc_thread(p); /* XXX */
		thread_reference(thread);
		proc_unlock(p);
	} else {
		thread = current_thread();
	}

	context.vc_thread = thread;
	context.vc_ucred = fp->fp_glob->fg_cred;

	/* Set the vflag for drain */
	fileproc_modify_vflags(fp, FPV_DRAIN, FALSE);

	while (os_ref_get_count(&fp->fp_iocount) > 1) {
		lck_mtx_convert_spin(&fdp->fd_lock);

		fo_drain(fp, &context);
		if ((fp->fp_flags & FP_INSELECT) == FP_INSELECT) {
			struct select_set *selset;

			if (fp->fp_guard_attrs) {
				selset = fp->fp_guard->fpg_wset;
			} else {
				selset = fp->fp_wset;
			}
			if (waitq_wakeup64_all(selset, NO_EVENT64,
			    THREAD_INTERRUPTED, WAITQ_WAKEUP_DEFAULT) == KERN_INVALID_ARGUMENT) {
				panic("bad wait queue for waitq_wakeup64_all %p (%sfp:%p)",
				    selset, fp->fp_guard_attrs ? "guarded " : "", fp);
			}
		}
		if ((fp->fp_flags & FP_SELCONFLICT) == FP_SELCONFLICT) {
			if (waitq_wakeup64_all(&select_conflict_queue, NO_EVENT64,
			    THREAD_INTERRUPTED, WAITQ_WAKEUP_DEFAULT) == KERN_INVALID_ARGUMENT) {
				panic("bad select_conflict_queue");
			}
		}
		fdp->fd_fpdrainwait = 1;
		msleep(&fdp->fd_fpdrainwait, &fdp->fd_lock, PRIBIO, "fpdrain", NULL);
	}
#if DIAGNOSTIC
	if ((fp->fp_flags & FP_INSELECT) != 0) {
		panic("FP_INSELECT set on drained fp");
	}
#endif
	if ((fp->fp_flags & FP_SELCONFLICT) == FP_SELCONFLICT) {
		fp->fp_flags &= ~FP_SELCONFLICT;
	}

	if (!is_current_proc) {
		thread_deallocate(thread);
	}
}


int
fp_close_and_unlock(proc_t p, kauth_cred_t cred, int fd, struct fileproc *fp, int flags)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileglob *fg = fp->fp_glob;

#if DIAGNOSTIC
	proc_fdlock_assert(p, LCK_MTX_ASSERT_OWNED);
#endif

	/*
	 * Keep most people from finding the filedesc while we are closing it.
	 *
	 * Callers are:
	 *
	 * - dup2() which always waits for UF_RESERVED to clear
	 *
	 * - close/guarded_close/... who will fail the fileproc lookup if
	 *   UF_RESERVED is set,
	 *
	 * - fdexec()/fdfree() who only run once all threads in the proc
	 *   are properly canceled, hence no fileproc in this proc should
	 *   be in flux.
	 *
	 * Which means that neither UF_RESERVED nor UF_CLOSING should be set.
	 *
	 * Callers of fp_get_noref_locked_with_iocount() can still find
	 * this entry so that they can drop their I/O reference despite
	 * not having remembered the fileproc pointer (namely select() and
	 * file_drop()).
	 */
	if (p->p_fd.fd_ofileflags[fd] & (UF_RESERVED | UF_CLOSING)) {
		panic("%s: called with fileproc in flux (%d/:%p)",
		    __func__, fd, fp);
	}
	p->p_fd.fd_ofileflags[fd] |= (UF_RESERVED | UF_CLOSING);

	if ((fp->fp_flags & FP_AIOISSUED) ||
#if CONFIG_MACF
	    (FILEGLOB_DTYPE(fg) == DTYPE_VNODE)
#else
	    kauth_authorize_fileop_has_listeners()
#endif
	    ) {
		proc_fdunlock(p);

		if (FILEGLOB_DTYPE(fg) == DTYPE_VNODE) {
			/*
			 * call out to allow 3rd party notification of close.
			 * Ignore result of kauth_authorize_fileop call.
			 */
#if CONFIG_MACF
			mac_file_notify_close(cred, fp->fp_glob);
#else
			(void)cred;
#endif

			if (kauth_authorize_fileop_has_listeners() &&
			    vnode_getwithref((vnode_t)fg_get_data(fg)) == 0) {
				u_int   fileop_flags = 0;
				if (fg->fg_flag & FWASWRITTEN) {
					fileop_flags |= KAUTH_FILEOP_CLOSE_MODIFIED;
				}
				kauth_authorize_fileop(fg->fg_cred, KAUTH_FILEOP_CLOSE,
				    (uintptr_t)fg_get_data(fg), (uintptr_t)fileop_flags);

				vnode_put((vnode_t)fg_get_data(fg));
			}
		}

		if (fp->fp_flags & FP_AIOISSUED) {
			/*
			 * cancel all async IO requests that can be cancelled.
			 */
			_aio_close( p, fd );
		}

		proc_fdlock(p);
	}

	if (fd < fdp->fd_knlistsize) {
		knote_fdclose(p, fd);
	}

	fileproc_drain(p, fp);

	if (flags & FD_DUP2RESV) {
		fdp->fd_ofiles[fd] = NULL;
		fdp->fd_ofileflags[fd] &= ~UF_CLOSING;
	} else {
		fdrelse(p, fd);
	}

	proc_fdunlock(p);

	if (ENTR_SHOULDTRACE && FILEGLOB_DTYPE(fg) == DTYPE_SOCKET) {
		KERNEL_ENERGYTRACE(kEnTrActKernSocket, DBG_FUNC_END,
		    fd, 0, (int64_t)VM_KERNEL_ADDRPERM(fg_get_data(fg)));
	}

	fileproc_free(fp);

	return fg_drop(p, fg);
}

/*
 * dupfdopen
 *
 * Description:	Duplicate the specified descriptor to a free descriptor;
 *		this is the second half of fdopen(), above.
 *
 * Parameters:	p				current process pointer
 *		indx				fd to dup to
 *		dfd				fd to dup from
 *		mode				mode to set on new fd
 *		error				command code
 *
 * Returns:	0				Success
 *		EBADF				Source fd is bad
 *		EACCES				Requested mode not allowed
 *		!0				'error', if not ENODEV or
 *						ENXIO
 *
 * Notes:	XXX This is not thread safe; see fdopen() above
 */
int
dupfdopen(proc_t p, int indx, int dfd, int flags, int error)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *wfp;
	struct fileproc *fp;
#if CONFIG_MACF
	int myerror;
#endif

	/*
	 * If the to-be-dup'd fd number is greater than the allowed number
	 * of file descriptors, or the fd to be dup'd has already been
	 * closed, reject.  Note, check for new == old is necessary as
	 * falloc could allocate an already closed to-be-dup'd descriptor
	 * as the new descriptor.
	 */
	proc_fdlock(p);

	fp = fdp->fd_ofiles[indx];
	if (dfd < 0 || dfd >= fdp->fd_nfiles ||
	    (wfp = fdp->fd_ofiles[dfd]) == NULL || wfp == fp ||
	    (fdp->fd_ofileflags[dfd] & UF_RESERVED)) {
		proc_fdunlock(p);
		return EBADF;
	}
#if CONFIG_MACF
	myerror = mac_file_check_dup(kauth_cred_get(), wfp->fp_glob, dfd);
	if (myerror) {
		proc_fdunlock(p);
		return myerror;
	}
#endif
	/*
	 * There are two cases of interest here.
	 *
	 * For ENODEV simply dup (dfd) to file descriptor
	 * (indx) and return.
	 *
	 * For ENXIO steal away the file structure from (dfd) and
	 * store it in (indx).  (dfd) is effectively closed by
	 * this operation.
	 *
	 * Any other error code is just returned.
	 */
	switch (error) {
	case ENODEV:
		if (fp_isguarded(wfp, GUARD_DUP)) {
			proc_fdunlock(p);
			return EPERM;
		}

		/*
		 * Check that the mode the file is being opened for is a
		 * subset of the mode of the existing descriptor.
		 */
		if (((flags & (FREAD | FWRITE)) | wfp->f_flag) != wfp->f_flag) {
			proc_fdunlock(p);
			return EACCES;
		}
		if (indx >= fdp->fd_afterlast) {
			fdp->fd_afterlast = indx + 1;
		}

		if (fp->fp_glob) {
			fg_free(fp->fp_glob);
		}
		fg_ref(p, wfp->fp_glob);
		fp->fp_glob = wfp->fp_glob;
		/*
		 * Historically, open(/dev/fd/<n>) preserves close on fork/exec,
		 * unlike dup(), dup2() or fcntl(F_DUPFD).
		 *
		 * open1() already handled O_CLO{EXEC,FORK}
		 */
		fp->fp_flags |= (wfp->fp_flags & (FP_CLOFORK | FP_CLOEXEC));

		procfdtbl_releasefd(p, indx, NULL);
		fp_drop(p, indx, fp, 1);
		proc_fdunlock(p);
		return 0;

	default:
		proc_fdunlock(p);
		return error;
	}
	/* NOTREACHED */
}


#pragma mark KPIS (sys/file.h)

/*
 * fg_get_vnode
 *
 * Description:	Return vnode associated with the file structure, if
 *		any.  The lifetime of the returned vnode is bound to
 *		the lifetime of the file structure.
 *
 * Parameters:	fg				Pointer to fileglob to
 *						inspect
 *
 * Returns:	vnode_t
 */
vnode_t
fg_get_vnode(struct fileglob *fg)
{
	if (FILEGLOB_DTYPE(fg) == DTYPE_VNODE) {
		return (vnode_t)fg_get_data(fg);
	} else {
		return NULL;
	}
}


/*
 * fp_getfvp
 *
 * Description:	Get fileproc and vnode pointer for a given fd from the per
 *		process open file table of the specified process, and if
 *		successful, increment the fp_iocount
 *
 * Parameters:	p				Process in which fd lives
 *		fd				fd to get information for
 *		resultfp			Pointer to result fileproc
 *						pointer area, or 0 if none
 *		resultvp			Pointer to result vnode pointer
 *						area, or 0 if none
 *
 * Returns:	0				Success
 *		EBADF				Bad file descriptor
 *		ENOTSUP				fd does not refer to a vnode
 *
 * Implicit returns:
 *		*resultfp (modified)		Fileproc pointer
 *		*resultvp (modified)		vnode pointer
 *
 * Notes:	The resultfp and resultvp fields are optional, and may be
 *		independently specified as NULL to skip returning information
 *
 * Locks:	Internally takes and releases proc_fdlock
 */
int
fp_getfvp(proc_t p, int fd, struct fileproc **resultfp, struct vnode **resultvp)
{
	struct fileproc *fp;
	int error;

	error = fp_get_ftype(p, fd, DTYPE_VNODE, ENOTSUP, &fp);
	if (error == 0) {
		if (resultfp) {
			*resultfp = fp;
		}
		if (resultvp) {
			*resultvp = (struct vnode *)fp_get_data(fp);
		}
	}

	return error;
}


/*
 * fp_get_pipe_id
 *
 * Description:	Get pipe id for a given fd from the per process open file table
 *		of the specified process.
 *
 * Parameters:	p				Process in which fd lives
 *		fd				fd to get information for
 *		result_pipe_id			Pointer to result pipe id
 *
 * Returns:	0				Success
 *		EIVAL				NULL pointer arguments passed
 *		fp_lookup:EBADF			Bad file descriptor
 *		ENOTSUP				fd does not refer to a pipe
 *
 * Implicit returns:
 *		*result_pipe_id (modified)	pipe id
 *
 * Locks:	Internally takes and releases proc_fdlock
 */
int
fp_get_pipe_id(proc_t p, int fd, uint64_t *result_pipe_id)
{
	struct fileproc *fp = FILEPROC_NULL;
	struct fileglob *fg = NULL;
	int error = 0;

	if (p == NULL || result_pipe_id == NULL) {
		return EINVAL;
	}

	proc_fdlock(p);
	if ((error = fp_lookup(p, fd, &fp, 1))) {
		proc_fdunlock(p);
		return error;
	}
	fg = fp->fp_glob;

	if (FILEGLOB_DTYPE(fg) == DTYPE_PIPE) {
		*result_pipe_id = pipe_id((struct pipe*)fg_get_data(fg));
	} else {
		error = ENOTSUP;
	}

	fp_drop(p, fd, fp, 1);
	proc_fdunlock(p);
	return error;
}


/*
 * file_vnode
 *
 * Description:	Given an fd, look it up in the current process's per process
 *		open file table, and return its internal vnode pointer.
 *
 * Parameters:	fd				fd to obtain vnode from
 *		vpp				pointer to vnode return area
 *
 * Returns:	0				Success
 *		EINVAL				The fd does not refer to a
 *						vnode fileproc entry
 *	fp_lookup:EBADF				Bad file descriptor
 *
 * Implicit returns:
 *		*vpp (modified)			Returned vnode pointer
 *
 * Locks:	This function internally takes and drops the proc_fdlock for
 *		the current process
 *
 * Notes:	If successful, this function increments the fp_iocount on the
 *		fd's corresponding fileproc.
 *
 *		The fileproc referenced is not returned; because of this, care
 *		must be taken to not drop the last reference (e.g. by closing
 *		the file).  This is inherently unsafe, since the reference may
 *		not be recoverable from the vnode, if there is a subsequent
 *		close that destroys the associate fileproc.  The caller should
 *		therefore retain their own reference on the fileproc so that
 *		the fp_iocount can be dropped subsequently.  Failure to do this
 *		can result in the returned pointer immediately becoming invalid
 *		following the call.
 *
 *		Use of this function is discouraged.
 */
int
file_vnode(int fd, struct vnode **vpp)
{
	return file_vnode_withvid(fd, vpp, NULL);
}


/*
 * file_vnode_withvid
 *
 * Description:	Given an fd, look it up in the current process's per process
 *		open file table, and return its internal vnode pointer.
 *
 * Parameters:	fd				fd to obtain vnode from
 *		vpp				pointer to vnode return area
 *		vidp				pointer to vid of the returned vnode
 *
 * Returns:	0				Success
 *		EINVAL				The fd does not refer to a
 *						vnode fileproc entry
 *	fp_lookup:EBADF				Bad file descriptor
 *
 * Implicit returns:
 *		*vpp (modified)			Returned vnode pointer
 *
 * Locks:	This function internally takes and drops the proc_fdlock for
 *		the current process
 *
 * Notes:	If successful, this function increments the fp_iocount on the
 *		fd's corresponding fileproc.
 *
 *		The fileproc referenced is not returned; because of this, care
 *		must be taken to not drop the last reference (e.g. by closing
 *		the file).  This is inherently unsafe, since the reference may
 *		not be recoverable from the vnode, if there is a subsequent
 *		close that destroys the associate fileproc.  The caller should
 *		therefore retain their own reference on the fileproc so that
 *		the fp_iocount can be dropped subsequently.  Failure to do this
 *		can result in the returned pointer immediately becoming invalid
 *		following the call.
 *
 *		Use of this function is discouraged.
 */
int
file_vnode_withvid(int fd, struct vnode **vpp, uint32_t *vidp)
{
	struct fileproc *fp;
	int error;

	error = fp_get_ftype(current_proc(), fd, DTYPE_VNODE, EINVAL, &fp);
	if (error == 0) {
		if (vpp) {
			*vpp = (struct vnode *)fp_get_data(fp);
		}
		if (vidp) {
			*vidp = vnode_vid((struct vnode *)fp_get_data(fp));
		}
	}
	return error;
}

/*
 * file_socket
 *
 * Description:	Given an fd, look it up in the current process's per process
 *		open file table, and return its internal socket pointer.
 *
 * Parameters:	fd				fd to obtain vnode from
 *		sp				pointer to socket return area
 *
 * Returns:	0				Success
 *		ENOTSOCK			Not a socket
 *		fp_lookup:EBADF			Bad file descriptor
 *
 * Implicit returns:
 *		*sp (modified)			Returned socket pointer
 *
 * Locks:	This function internally takes and drops the proc_fdlock for
 *		the current process
 *
 * Notes:	If successful, this function increments the fp_iocount on the
 *		fd's corresponding fileproc.
 *
 *		The fileproc referenced is not returned; because of this, care
 *		must be taken to not drop the last reference (e.g. by closing
 *		the file).  This is inherently unsafe, since the reference may
 *		not be recoverable from the socket, if there is a subsequent
 *		close that destroys the associate fileproc.  The caller should
 *		therefore retain their own reference on the fileproc so that
 *		the fp_iocount can be dropped subsequently.  Failure to do this
 *		can result in the returned pointer immediately becoming invalid
 *		following the call.
 *
 *		Use of this function is discouraged.
 */
int
file_socket(int fd, struct socket **sp)
{
	struct fileproc *fp;
	int error;

	error = fp_get_ftype(current_proc(), fd, DTYPE_SOCKET, ENOTSOCK, &fp);
	if (error == 0) {
		if (sp) {
			*sp = (struct socket *)fp_get_data(fp);
		}
	}
	return error;
}


/*
 * file_flags
 *
 * Description:	Given an fd, look it up in the current process's per process
 *		open file table, and return its fileproc's flags field.
 *
 * Parameters:	fd				fd whose flags are to be
 *						retrieved
 *		flags				pointer to flags data area
 *
 * Returns:	0				Success
 *		ENOTSOCK			Not a socket
 *		fp_lookup:EBADF			Bad file descriptor
 *
 * Implicit returns:
 *		*flags (modified)		Returned flags field
 *
 * Locks:	This function internally takes and drops the proc_fdlock for
 *		the current process
 */
int
file_flags(int fd, int *flags)
{
	proc_t p = current_proc();
	struct fileproc *fp;
	int error = EBADF;

	proc_fdlock_spin(p);
	fp = fp_get_noref_locked(p, fd);
	if (fp) {
		*flags = (int)fp->f_flag;
		error = 0;
	}
	proc_fdunlock(p);

	return error;
}


/*
 * file_drop
 *
 * Description:	Drop an iocount reference on an fd, and wake up any waiters
 *		for draining (i.e. blocked in fileproc_drain() called during
 *		the last attempt to close a file).
 *
 * Parameters:	fd				fd on which an ioreference is
 *						to be dropped
 *
 * Returns:	0				Success
 *
 * Description:	Given an fd, look it up in the current process's per process
 *		open file table, and drop it's fileproc's fp_iocount by one
 *
 * Notes:	This is intended as a corresponding operation to the functions
 *		file_vnode() and file_socket() operations.
 *
 *		If the caller can't possibly hold an I/O reference,
 *		this function will panic the kernel rather than allowing
 *		for memory corruption. Callers should always call this
 *		because they acquired an I/O reference on this file before.
 *
 *		Use of this function is discouraged.
 */
int
file_drop(int fd)
{
	struct fileproc *fp;
	proc_t p = current_proc();
	struct filedesc *fdp = &p->p_fd;
	int     needwakeup = 0;

	proc_fdlock_spin(p);
	fp = fp_get_noref_locked_with_iocount(p, fd);

	if (1 == os_ref_release_locked(&fp->fp_iocount)) {
		if (fp->fp_flags & FP_SELCONFLICT) {
			fp->fp_flags &= ~FP_SELCONFLICT;
		}

		if (fdp->fd_fpdrainwait) {
			fdp->fd_fpdrainwait = 0;
			needwakeup = 1;
		}
	}
	proc_fdunlock(p);

	if (needwakeup) {
		wakeup(&fdp->fd_fpdrainwait);
	}
	return 0;
}


#pragma mark syscalls

#ifndef HFS_GET_BOOT_INFO
#define HFS_GET_BOOT_INFO   (FCNTL_FS_SPECIFIC_BASE + 0x00004)
#endif

#ifndef HFS_SET_BOOT_INFO
#define HFS_SET_BOOT_INFO   (FCNTL_FS_SPECIFIC_BASE + 0x00005)
#endif

#ifndef APFSIOC_REVERT_TO_SNAPSHOT
#define APFSIOC_REVERT_TO_SNAPSHOT  _IOW('J', 1, u_int64_t)
#endif

#ifndef APFSIOC_IS_GRAFT_SUPPORTED
#define APFSIOC_IS_GRAFT_SUPPORTED _IO('J', 133)
#endif

#define CHECK_ADD_OVERFLOW_INT64L(x, y) \
	        (((((x) > 0) && ((y) > 0) && ((x) > LLONG_MAX - (y))) || \
	        (((x) < 0) && ((y) < 0) && ((x) < LLONG_MIN - (y)))) \
	        ? 1 : 0)

/*
 * sys_getdtablesize
 *
 * Description:	Returns the per process maximum size of the descriptor table
 *
 * Parameters:	p				Process being queried
 *		retval				Pointer to the call return area
 *
 * Returns:	0				Success
 *
 * Implicit returns:
 *		*retval (modified)		Size of dtable
 */
int
sys_getdtablesize(proc_t p, __unused struct getdtablesize_args *uap, int32_t *retval)
{
	*retval = proc_limitgetcur_nofile(p);
	return 0;
}


/*
 * check_file_seek_range
 *
 * Description: Checks if seek offsets are in the range of 0 to LLONG_MAX.
 *
 * Parameters:  fl		Flock structure.
 *		cur_file_offset	Current offset in the file.
 *
 * Returns:     0               on Success.
 *		EOVERFLOW	on overflow.
 *		EINVAL          on offset less than zero.
 */

static int
check_file_seek_range(struct flock *fl, off_t cur_file_offset)
{
	if (fl->l_whence == SEEK_CUR) {
		/* Check if the start marker is beyond LLONG_MAX. */
		if (CHECK_ADD_OVERFLOW_INT64L(fl->l_start, cur_file_offset)) {
			/* Check if start marker is negative */
			if (fl->l_start < 0) {
				return EINVAL;
			}
			return EOVERFLOW;
		}
		/* Check if the start marker is negative. */
		if (fl->l_start + cur_file_offset < 0) {
			return EINVAL;
		}
		/* Check if end marker is beyond LLONG_MAX. */
		if ((fl->l_len > 0) && (CHECK_ADD_OVERFLOW_INT64L(fl->l_start +
		    cur_file_offset, fl->l_len - 1))) {
			return EOVERFLOW;
		}
		/* Check if the end marker is negative. */
		if ((fl->l_len <= 0) && (fl->l_start + cur_file_offset +
		    fl->l_len < 0)) {
			return EINVAL;
		}
	} else if (fl->l_whence == SEEK_SET) {
		/* Check if the start marker is negative. */
		if (fl->l_start < 0) {
			return EINVAL;
		}
		/* Check if the end marker is beyond LLONG_MAX. */
		if ((fl->l_len > 0) &&
		    CHECK_ADD_OVERFLOW_INT64L(fl->l_start, fl->l_len - 1)) {
			return EOVERFLOW;
		}
		/* Check if the end marker is negative. */
		if ((fl->l_len < 0) && fl->l_start + fl->l_len < 0) {
			return EINVAL;
		}
	}
	return 0;
}


/*
 * sys_dup
 *
 * Description:	Duplicate a file descriptor.
 *
 * Parameters:	p				Process performing the dup
 *		uap->fd				The fd to dup
 *		retval				Pointer to the call return area
 *
 * Returns:	0				Success
 *		!0				Errno
 *
 * Implicit returns:
 *		*retval (modified)		The new descriptor
 */
int
sys_dup(proc_t p, struct dup_args *uap, int32_t *retval)
{
	int old = uap->fd;
	int new, error;
	struct fileproc *fp;
	kauth_cred_t p_cred;

	proc_fdlock(p);
	if ((error = fp_lookup(p, old, &fp, 1))) {
		proc_fdunlock(p);
		return error;
	}
	if (fp_isguarded(fp, GUARD_DUP)) {
		error = fp_guard_exception(p, old, fp, kGUARD_EXC_DUP);
		(void) fp_drop(p, old, fp, 1);
		proc_fdunlock(p);
		return error;
	}
	if ((error = fdalloc(p, 0, &new))) {
		fp_drop(p, old, fp, 1);
		proc_fdunlock(p);
		return error;
	}
	p_cred = current_cached_proc_cred(p);
	error = finishdup(p, p_cred, old, new, 0, retval);

	if (ENTR_SHOULDTRACE && FILEGLOB_DTYPE(fp->fp_glob) == DTYPE_SOCKET) {
		KERNEL_ENERGYTRACE(kEnTrActKernSocket, DBG_FUNC_START,
		    new, 0, (int64_t)VM_KERNEL_ADDRPERM(fp_get_data(fp)));
	}

	fp_drop(p, old, fp, 1);
	proc_fdunlock(p);

	return error;
}

/*
 * sys_dup2
 *
 * Description:	Duplicate a file descriptor to a particular value.
 *
 * Parameters:	p				Process performing the dup
 *		uap->from			The fd to dup
 *		uap->to				The fd to dup it to
 *		retval				Pointer to the call return area
 *
 * Returns:	0				Success
 *		!0				Errno
 *
 * Implicit returns:
 *		*retval (modified)		The new descriptor
 */
int
sys_dup2(proc_t p, struct dup2_args *uap, int32_t *retval)
{
	kauth_cred_t p_cred = current_cached_proc_cred(p);

	return dup2(p, p_cred, uap->from, uap->to, retval);
}

int
dup2(proc_t p, kauth_cred_t p_cred, int old, int new, int *retval)
{
	struct filedesc *fdp = &p->p_fd;
	struct fileproc *fp, *nfp;
	int i, error;

	proc_fdlock(p);

startover:
	if ((error = fp_lookup(p, old, &fp, 1))) {
		proc_fdunlock(p);
		return error;
	}
	if (fp_isguarded(fp, GUARD_DUP)) {
		error = fp_guard_exception(p, old, fp, kGUARD_EXC_DUP);
		(void) fp_drop(p, old, fp, 1);
		proc_fdunlock(p);
		return error;
	}
	if (new < 0 || new >= proc_limitgetcur_nofile(p)) {
		fp_drop(p, old, fp, 1);
		proc_fdunlock(p);
		return EBADF;
	}
	if (old == new) {
		fp_drop(p, old, fp, 1);
		*retval = new;
		proc_fdunlock(p);
		return 0;
	}
	if (new < 0 || new >= fdp->fd_nfiles) {
		if ((error = fdalloc(p, new, &i))) {
			fp_drop(p, old, fp, 1);
			proc_fdunlock(p);
			return error;
		}
		if (new != i) {
			fdrelse(p, i);
			goto closeit;
		}
	} else {
closeit:
		if ((fdp->fd_ofileflags[new] & UF_RESERVED) == UF_RESERVED) {
			fp_drop(p, old, fp, 1);
			procfdtbl_waitfd(p, new);
#if DIAGNOSTIC
			proc_fdlock_assert(p, LCK_MTX_ASSERT_OWNED);
#endif
			goto startover;
		}

		if ((nfp = fdp->fd_ofiles[new]) != NULL) {
			if (fp_isguarded(nfp, GUARD_CLOSE)) {
				fp_drop(p, old, fp, 1);
				error = fp_guard_exception(p,
				    new, nfp, kGUARD_EXC_CLOSE);
				proc_fdunlock(p);
				return error;
			}
			(void)fp_close_and_unlock(p, p_cred, new, nfp, FD_DUP2RESV);
			proc_fdlock(p);
			assert(fdp->fd_ofileflags[new] & UF_RESERVED);
		} else {
#if DIAGNOSTIC
			if (fdp->fd_ofiles[new] != NULL) {
				panic("dup2: no ref on fileproc %d", new);
			}
#endif
			procfdtbl_reservefd(p, new);
		}
	}
#if DIAGNOSTIC
	if (fdp->fd_ofiles[new] != 0) {
		panic("dup2: overwriting fd_ofiles with new %d", new);
	}
	if ((fdp->fd_ofileflags[new] & UF_RESERVED) == 0) {
		panic("dup2: unreserved fileflags with new %d", new);
	}
#endif
	error = finishdup(p, p_cred, old, new, 0, retval);
	fp_drop(p, old, fp, 1);
	proc_fdunlock(p);

	return error;
}


/*
 * fcntl
 *
 * Description:	The file control system call.
 *
 * Parameters:	p			Process performing the fcntl
 *		uap->fd				The fd to operate against
 *		uap->cmd			The command to perform
 *		uap->arg			Pointer to the command argument
 *		retval				Pointer to the call return area
 *
 * Returns:	0			Success
 *		!0				Errno (see fcntl_nocancel)
 *
 * Implicit returns:
 *		*retval (modified)		fcntl return value (if any)
 *
 * Notes:	This system call differs from fcntl_nocancel() in that it
 *		tests for cancellation prior to performing a potentially
 *		blocking operation.
 */
int
sys_fcntl(proc_t p, struct fcntl_args *uap, int32_t *retval)
{
	__pthread_testcancel(1);
	return sys_fcntl_nocancel(p, (struct fcntl_nocancel_args *)uap, retval);
}

#define ACCOUNT_OPENFROM_ENTITLEMENT \
	"com.apple.private.vfs.role-account-openfrom"

/*
 * sys_fcntl_nocancel
 *
 * Description:	A non-cancel-testing file control system call.
 *
 * Parameters:	p				Process performing the fcntl
 *		uap->fd				The fd to operate against
 *		uap->cmd			The command to perform
 *		uap->arg			Pointer to the command argument
 *		retval				Pointer to the call return area
 *
 * Returns:	0				Success
 *		EINVAL
 *	fp_lookup:EBADF				Bad file descriptor
 * [F_DUPFD]
 *	fdalloc:EMFILE
 *	fdalloc:ENOMEM
 *	finishdup:EBADF
 *	finishdup:ENOMEM
 * [F_SETOWN]
 *		ESRCH
 * [F_SETLK]
 *		EBADF
 *		EOVERFLOW
 *	copyin:EFAULT
 *	vnode_getwithref:???
 *	VNOP_ADVLOCK:???
 *	msleep:ETIMEDOUT
 * [F_GETLK]
 *		EBADF
 *		EOVERFLOW
 *	copyin:EFAULT
 *	copyout:EFAULT
 *	vnode_getwithref:???
 *	VNOP_ADVLOCK:???
 * [F_PREALLOCATE]
 *		EBADF
 *		EFBIG
 *		EINVAL
 *		ENOSPC
 *	copyin:EFAULT
 *	copyout:EFAULT
 *	vnode_getwithref:???
 *	VNOP_ALLOCATE:???
 * [F_SETSIZE,F_RDADVISE]
 *		EBADF
 *		EINVAL
 *	copyin:EFAULT
 *	vnode_getwithref:???
 * [F_RDAHEAD,F_NOCACHE]
 *		EBADF
 *	vnode_getwithref:???
 * [???]
 *
 * Implicit returns:
 *		*retval (modified)		fcntl return value (if any)
 */
#define SYS_FCNTL_DECLARE_VFS_CONTEXT(context) \
	struct vfs_context context = { \
	    .vc_thread = current_thread(), \
	    .vc_ucred = fp->f_cred, \
	}

static user_addr_t
sys_fnctl_parse_arg(proc_t p, user_long_t arg)
{
	/*
	 * Since the arg parameter is defined as a long but may be
	 * either a long or a pointer we must take care to handle
	 * sign extension issues.  Our sys call munger will sign
	 * extend a long when we are called from a 32-bit process.
	 * Since we can never have an address greater than 32-bits
	 * from a 32-bit process we lop off the top 32-bits to avoid
	 * getting the wrong address
	 */
	return proc_is64bit(p) ? arg : CAST_USER_ADDR_T((uint32_t)arg);
}

/* cleanup code common to fnctl functions, for when the fdlock is still held */
static int
sys_fcntl_out(proc_t p, int fd, struct fileproc *fp, int error)
{
	fp_drop(p, fd, fp, 1);
	proc_fdunlock(p);
	return error;
}

/* cleanup code common to fnctl acting on vnodes, once they unlocked the fdlock */
static int
sys_fcntl_outdrop(proc_t p, int fd, struct fileproc *fp, struct vnode *vp, int error)
{
#pragma unused(vp)

	AUDIT_ARG(vnpath_withref, vp, ARG_VNODE1);
	fp_drop(p, fd, fp, 0);
	return error;
}

typedef int (*sys_fnctl_handler_t)(proc_t p, int fd, int cmd, user_long_t arg,
    struct fileproc *fp, int32_t *retval);

typedef int (*sys_fnctl_vnode_handler_t)(proc_t p, int fd, int cmd,
    user_long_t arg, struct fileproc *fp, struct vnode *vp, int32_t *retval);

/*
 * SPI (private) for opening a file starting from a dir fd
 *
 * Note: do not inline to keep stack usage under control.
 */
__attribute__((noinline))
static int
sys_fcntl__OPENFROM(proc_t p, int fd, int cmd, user_long_t arg,
    struct fileproc *fp, struct vnode *vp, int32_t *retval)
{
#pragma unused(cmd)

	user_addr_t argp = sys_fnctl_parse_arg(p, arg);
	struct user_fopenfrom fopen;
	struct vnode_attr *va;
	struct nameidata *nd;
	int error, cmode;
	bool has_entitlement;

	/* Check if this isn't a valid file descriptor */
	if ((fp->f_flag & FREAD) == 0) {
		return sys_fcntl_out(p, fd, fp, EBADF);
	}
	proc_fdunlock(p);

	if (vnode_getwithref(vp)) {
		error = ENOENT;
		goto outdrop;
	}

	/* Only valid for directories */
	if (vp->v_type != VDIR) {
		vnode_put(vp);
		error = ENOTDIR;
		goto outdrop;
	}

	/*
	 * Only entitled apps may use the credentials of the thread
	 * that opened the file descriptor.
	 * Non-entitled threads will use their own context.
	 */
	has_entitlement = IOCurrentTaskHasEntitlement(ACCOUNT_OPENFROM_ENTITLEMENT);

	/* Get flags, mode and pathname arguments. */
	if (IS_64BIT_PROCESS(p)) {
		error = copyin(argp, &fopen, sizeof(fopen));
	} else {
		struct user32_fopenfrom fopen32;

		error = copyin(argp, &fopen32, sizeof(fopen32));
		fopen.o_flags = fopen32.o_flags;
		fopen.o_mode = fopen32.o_mode;
		fopen.o_pathname = CAST_USER_ADDR_T(fopen32.o_pathname);
	}
	if (error) {
		vnode_put(vp);
		goto outdrop;
	}

	/* open1() can have really deep stacks, so allocate those */
	va = kalloc_type(struct vnode_attr, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	nd = kalloc_type(struct nameidata, Z_WAITOK | Z_ZERO | Z_NOFAIL);

	AUDIT_ARG(fflags, fopen.o_flags);
	AUDIT_ARG(mode, fopen.o_mode);
	VATTR_INIT(va);
	/* Mask off all but regular access permissions */
	cmode = ((fopen.o_mode & ~p->p_fd.fd_cmask) & ALLPERMS) & ~S_ISTXT;
	VATTR_SET(va, va_mode, cmode & ACCESSPERMS);

	SYS_FCNTL_DECLARE_VFS_CONTEXT(context);

	/* Start the lookup relative to the file descriptor's vnode. */
	NDINIT(nd, LOOKUP, OP_OPEN, USEDVP | FOLLOW | AUDITVNPATH1, UIO_USERSPACE,
	    fopen.o_pathname, has_entitlement ? &context : vfs_context_current());
	nd->ni_dvp = vp;

	error = open1(has_entitlement ? &context : vfs_context_current(),
	    nd, fopen.o_flags, va, NULL, NULL, retval, AUTH_OPEN_NOAUTHFD);

	kfree_type(struct vnode_attr, va);
	kfree_type(struct nameidata, nd);

	vnode_put(vp);

outdrop:
	return sys_fcntl_outdrop(p, fd, fp, vp, error);
}

int
sys_fcntl_nocancel(proc_t p, struct fcntl_nocancel_args *uap, int32_t *retval)
{
	int fd = uap->fd;
	int cmd = uap->cmd;
	struct fileproc *fp;
	struct vnode *vp = NULLVP;      /* for AUDIT_ARG() at end */
	unsigned int oflags, nflags;
	int i, tmp, error, error2, flg = 0;
	struct flock fl = {};
	struct flocktimeout fltimeout;
	struct timespec *timeout = NULL;
	off_t offset;
	int newmin;
	daddr64_t lbn, bn;
	unsigned int fflag;
	user_addr_t argp;
	boolean_t is64bit;
	int has_entitlement = 0;
	kauth_cred_t p_cred;
	cs_blob_add_flags_t csblob_add_flags = 0;

	AUDIT_ARG(fd, uap->fd);
	AUDIT_ARG(cmd, uap->cmd);

	proc_fdlock(p);
	if ((error = fp_lookup(p, fd, &fp, 1))) {
		proc_fdunlock(p);
		return error;
	}

	SYS_FCNTL_DECLARE_VFS_CONTEXT(context);

	is64bit = proc_is64bit(p);
	if (is64bit) {
		argp = uap->arg;
	} else {
		/*
		 * Since the arg parameter is defined as a long but may be
		 * either a long or a pointer we must take care to handle
		 * sign extension issues.  Our sys call munger will sign
		 * extend a long when we are called from a 32-bit process.
		 * Since we can never have an address greater than 32-bits
		 * from a 32-bit process we lop off the top 32-bits to avoid
		 * getting the wrong address
		 */
		argp = CAST_USER_ADDR_T((uint32_t)uap->arg);
	}

#if CONFIG_MACF
	error = mac_file_check_fcntl(kauth_cred_get(), fp->fp_glob, cmd, uap->arg);
	if (error) {
		goto out;
	}
#endif

	switch (cmd) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:
		if (fp_isguarded(fp, GUARD_DUP)) {
			error = fp_guard_exception(p, fd, fp, kGUARD_EXC_DUP);
			goto out;
		}
		newmin = CAST_DOWN_EXPLICIT(int, uap->arg); /* arg is an int, so we won't lose bits */
		AUDIT_ARG(value32, newmin);
		if (newmin < 0 || newmin >= proc_limitgetcur_nofile(p)) {
			error = EINVAL;
			goto out;
		}
		if ((error = fdalloc(p, newmin, &i))) {
			goto out;
		}
		p_cred = current_cached_proc_cred(p);
		error = finishdup(p, p_cred, fd, i,
		    cmd == F_DUPFD_CLOEXEC ? FP_CLOEXEC : 0, retval);
		goto out;

	case F_GETFD:
		*retval = (fp->fp_flags & FP_CLOEXEC) ? FD_CLOEXEC : 0;
		error = 0;
		goto out;

	case F_SETFD:
		AUDIT_ARG(value32, (uint32_t)uap->arg);
		if (uap->arg & FD_CLOEXEC) {
			fp->fp_flags |= FP_CLOEXEC;
			error = 0;
		} else if (!fp->fp_guard_attrs) {
			fp->fp_flags &= ~FP_CLOEXEC;
			error = 0;
		} else {
			error = fp_guard_exception(p,
			    fd, fp, kGUARD_EXC_NOCLOEXEC);
		}
		goto out;

	case F_GETFL:
		fflag = fp->f_flag;
		if ((fflag & O_EVTONLY) && proc_disallow_rw_for_o_evtonly(p)) {
			/*
			 * We insert back F_READ so that conversion back to open flags with
			 * OFLAGS() will come out right. We only need to set 'FREAD' as the
			 * 'O_RDONLY' is always implied.
			 */
			fflag |= FREAD;
		}
		*retval = OFLAGS(fflag);
		error = 0;
		goto out;

	case F_SETFL:
		// FIXME (rdar://54898652)
		//
		// this code is broken if fnctl(F_SETFL), ioctl() are
		// called concurrently for the same fileglob.

		tmp = CAST_DOWN_EXPLICIT(int, uap->arg); /* arg is an int, so we won't lose bits */
		AUDIT_ARG(value32, tmp);

		os_atomic_rmw_loop(&fp->f_flag, oflags, nflags, relaxed, {
			nflags  = oflags & ~FCNTLFLAGS;
			nflags |= FFLAGS(tmp) & FCNTLFLAGS;
		});
		tmp = nflags & FNONBLOCK;
		error = fo_ioctl(fp, FIONBIO, (caddr_t)&tmp, &context);
		if (error) {
			goto out;
		}
		tmp = nflags & FASYNC;
		error = fo_ioctl(fp, FIOASYNC, (caddr_t)&tmp, &context);
		if (!error) {
			goto out;
		}
		os_atomic_andnot(&fp->f_flag, FNONBLOCK, relaxed);
		tmp = 0;
		(void)fo_ioctl(fp, FIONBIO, (caddr_t)&tmp, &context);
		goto out;

	case F_GETOWN:
		if (fp->f_type == DTYPE_SOCKET) {
			*retval = ((struct socket *)fp_get_data(fp))->so_pgid;
			error = 0;
			goto out;
		}
		error = fo_ioctl(fp, TIOCGPGRP, (caddr_t)retval, &context);
		*retval = -*retval;
		goto out;

	case F_SETOWN:
		tmp = CAST_DOWN_EXPLICIT(pid_t, uap->arg); /* arg is an int, so we won't lose bits */
		AUDIT_ARG(value32, tmp);
		if (fp->f_type == DTYPE_SOCKET) {
			((struct socket *)fp_get_data(fp))->so_pgid = tmp;
			error = 0;
			goto out;
		}
		if (fp->f_type == DTYPE_PIPE) {
			error =  fo_ioctl(fp, TIOCSPGRP, (caddr_t)&tmp, &context);
			goto out;
		}

		if (tmp <= 0) {
			tmp = -tmp;
		} else {
			proc_t p1 = proc_find(tmp);
			if (p1 == 0) {
				error = ESRCH;
				goto out;
			}
			tmp = (int)p1->p_pgrpid;
			proc_rele(p1);
		}
		error =  fo_ioctl(fp, TIOCSPGRP, (caddr_t)&tmp, &context);
		goto out;

	case F_SETNOSIGPIPE:
		tmp = CAST_DOWN_EXPLICIT(int, uap->arg);
		if (fp->f_type == DTYPE_SOCKET) {
#if SOCKETS
			error = sock_setsockopt((struct socket *)fp_get_data(fp),
			    SOL_SOCKET, SO_NOSIGPIPE, &tmp, sizeof(tmp));
#else
			error = EINVAL;
#endif
		} else {
			struct fileglob *fg = fp->fp_glob;

			lck_mtx_lock_spin(&fg->fg_lock);
			if (tmp) {
				fg->fg_lflags |= FG_NOSIGPIPE;
			} else {
				fg->fg_lflags &= ~FG_NOSIGPIPE;
			}
			lck_mtx_unlock(&fg->fg_lock);
			error = 0;
		}
		goto out;

	case F_GETNOSIGPIPE:
		if (fp->f_type == DTYPE_SOCKET) {
#if SOCKETS
			int retsize = sizeof(*retval);
			error = sock_getsockopt((struct socket *)fp_get_data(fp),
			    SOL_SOCKET, SO_NOSIGPIPE, retval, &retsize);
#else
			error = EINVAL;
#endif
		} else {
			*retval = (fp->fp_glob->fg_lflags & FG_NOSIGPIPE) ?
			    1 : 0;
			error = 0;
		}
		goto out;

	case F_SETCONFINED:
		/*
		 * If this is the only reference to this fglob in the process
		 * and it's already marked as close-on-fork then mark it as
		 * (immutably) "confined" i.e. any fd that points to it will
		 * forever be close-on-fork, and attempts to use an IPC
		 * mechanism to move the descriptor elsewhere will fail.
		 */
		if (CAST_DOWN_EXPLICIT(int, uap->arg)) {
			struct fileglob *fg = fp->fp_glob;

			lck_mtx_lock_spin(&fg->fg_lock);
			if (fg->fg_lflags & FG_CONFINED) {
				error = 0;
			} else if (1 != os_ref_get_count_raw(&fg->fg_count)) {
				error = EAGAIN; /* go close the dup .. */
			} else if (fp->fp_flags & FP_CLOFORK) {
				fg->fg_lflags |= FG_CONFINED;
				error = 0;
			} else {
				error = EBADF;  /* open without O_CLOFORK? */
			}
			lck_mtx_unlock(&fg->fg_lock);
		} else {
			/*
			 * Other subsystems may have built on the immutability
			 * of FG_CONFINED; clearing it may be tricky.
			 */
			error = EPERM;          /* immutable */
		}
		goto out;

	case F_GETCONFINED:
		*retval = (fp->fp_glob->fg_lflags & FG_CONFINED) ? 1 : 0;
		error = 0;
		goto out;

	case F_SETLKWTIMEOUT:
	case F_SETLKW:
	case F_OFD_SETLKWTIMEOUT:
	case F_OFD_SETLKW:
		flg |= F_WAIT;
		OS_FALLTHROUGH;

	case F_SETLK:
	case F_OFD_SETLK:
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);

		fflag = fp->f_flag;
		offset = fp->f_offset;
		proc_fdunlock(p);

		/* Copy in the lock structure */
		if (F_SETLKWTIMEOUT == cmd || F_OFD_SETLKWTIMEOUT == cmd) {
			error = copyin(argp, (caddr_t) &fltimeout, sizeof(fltimeout));
			if (error) {
				goto outdrop;
			}
			fl = fltimeout.fl;
			timeout = &fltimeout.timeout;
		} else {
			error = copyin(argp, (caddr_t)&fl, sizeof(fl));
			if (error) {
				goto outdrop;
			}
		}

		/* Check starting byte and ending byte for EOVERFLOW in SEEK_CUR */
		/* and ending byte for EOVERFLOW in SEEK_SET */
		error = check_file_seek_range(&fl, offset);
		if (error) {
			goto outdrop;
		}

		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}
		if (fl.l_whence == SEEK_CUR) {
			fl.l_start += offset;
		}

#if CONFIG_MACF
		error = mac_file_check_lock(kauth_cred_get(), fp->fp_glob,
		    F_SETLK, &fl);
		if (error) {
			(void)vnode_put(vp);
			goto outdrop;
		}
#endif

#if CONFIG_FILE_LEASES
		(void)vnode_breaklease(vp, O_WRONLY, vfs_context_current());
#endif

		switch (cmd) {
		case F_OFD_SETLK:
		case F_OFD_SETLKW:
		case F_OFD_SETLKWTIMEOUT:
			flg |= F_OFD_LOCK;
			if (fp->fp_glob->fg_lflags & FG_CONFINED) {
				flg |= F_CONFINED;
			}
			switch (fl.l_type) {
			case F_RDLCK:
				if ((fflag & FREAD) == 0) {
					error = EBADF;
					break;
				}
				error = VNOP_ADVLOCK(vp, ofd_to_id(fp->fp_glob),
				    F_SETLK, &fl, flg, &context, timeout);
				break;
			case F_WRLCK:
				if ((fflag & FWRITE) == 0) {
					error = EBADF;
					break;
				}
				error = VNOP_ADVLOCK(vp, ofd_to_id(fp->fp_glob),
				    F_SETLK, &fl, flg, &context, timeout);
				break;
			case F_UNLCK:
				error = VNOP_ADVLOCK(vp, ofd_to_id(fp->fp_glob),
				    F_UNLCK, &fl, F_OFD_LOCK, &context,
				    timeout);
				break;
			default:
				error = EINVAL;
				break;
			}
			if (0 == error &&
			    (F_RDLCK == fl.l_type || F_WRLCK == fl.l_type)) {
				struct fileglob *fg = fp->fp_glob;

				/*
				 * arrange F_UNLCK on last close (once
				 * set, FG_HAS_OFDLOCK is immutable)
				 */
				if ((fg->fg_lflags & FG_HAS_OFDLOCK) == 0) {
					lck_mtx_lock_spin(&fg->fg_lock);
					fg->fg_lflags |= FG_HAS_OFDLOCK;
					lck_mtx_unlock(&fg->fg_lock);
				}
			}
			break;
		default:
			flg |= F_POSIX;
			switch (fl.l_type) {
			case F_RDLCK:
				if ((fflag & FREAD) == 0) {
					error = EBADF;
					break;
				}
				// XXX UInt32 unsafe for LP64 kernel
				os_atomic_or(&p->p_ladvflag, P_LADVLOCK, relaxed);
				error = VNOP_ADVLOCK(vp, (caddr_t)p,
				    F_SETLK, &fl, flg, &context, timeout);
				break;
			case F_WRLCK:
				if ((fflag & FWRITE) == 0) {
					error = EBADF;
					break;
				}
				// XXX UInt32 unsafe for LP64 kernel
				os_atomic_or(&p->p_ladvflag, P_LADVLOCK, relaxed);
				error = VNOP_ADVLOCK(vp, (caddr_t)p,
				    F_SETLK, &fl, flg, &context, timeout);
				break;
			case F_UNLCK:
				error = VNOP_ADVLOCK(vp, (caddr_t)p,
				    F_UNLCK, &fl, F_POSIX, &context, timeout);
				break;
			default:
				error = EINVAL;
				break;
			}
			break;
		}
		(void) vnode_put(vp);
		goto outdrop;

	case F_GETLK:
	case F_OFD_GETLK:
	case F_GETLKPID:
	case F_OFD_GETLKPID:
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);

		offset = fp->f_offset;
		proc_fdunlock(p);

		/* Copy in the lock structure */
		error = copyin(argp, (caddr_t)&fl, sizeof(fl));
		if (error) {
			goto outdrop;
		}

		/* Check starting byte and ending byte for EOVERFLOW in SEEK_CUR */
		/* and ending byte for EOVERFLOW in SEEK_SET */
		error = check_file_seek_range(&fl, offset);
		if (error) {
			goto outdrop;
		}

		if ((fl.l_whence == SEEK_SET) && (fl.l_start < 0)) {
			error = EINVAL;
			goto outdrop;
		}

		switch (fl.l_type) {
		case F_RDLCK:
		case F_UNLCK:
		case F_WRLCK:
			break;
		default:
			error = EINVAL;
			goto outdrop;
		}

		switch (fl.l_whence) {
		case SEEK_CUR:
		case SEEK_SET:
		case SEEK_END:
			break;
		default:
			error = EINVAL;
			goto outdrop;
		}

		if ((error = vnode_getwithref(vp)) == 0) {
			if (fl.l_whence == SEEK_CUR) {
				fl.l_start += offset;
			}

#if CONFIG_MACF
			error = mac_file_check_lock(kauth_cred_get(), fp->fp_glob,
			    cmd, &fl);
			if (error == 0)
#endif
			switch (cmd) {
			case F_OFD_GETLK:
				error = VNOP_ADVLOCK(vp, ofd_to_id(fp->fp_glob),
				    F_GETLK, &fl, F_OFD_LOCK, &context, NULL);
				break;
			case F_OFD_GETLKPID:
				error = VNOP_ADVLOCK(vp, ofd_to_id(fp->fp_glob),
				    F_GETLKPID, &fl, F_OFD_LOCK, &context, NULL);
				break;
			default:
				error = VNOP_ADVLOCK(vp, (caddr_t)p,
				    cmd, &fl, F_POSIX, &context, NULL);
				break;
			}

			(void)vnode_put(vp);

			if (error == 0) {
				error = copyout((caddr_t)&fl, argp, sizeof(fl));
			}
		}
		goto outdrop;

	case F_PREALLOCATE: {
		fstore_t alloc_struct;    /* structure for allocate command */
		u_int32_t alloc_flags = 0;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		/* make sure that we have write permission */
		if ((fp->f_flag & FWRITE) == 0) {
			error = EBADF;
			goto outdrop;
		}

		error = copyin(argp, (caddr_t)&alloc_struct, sizeof(alloc_struct));
		if (error) {
			goto outdrop;
		}

		/* now set the space allocated to 0 */
		alloc_struct.fst_bytesalloc = 0;

		/*
		 * Do some simple parameter checking
		 */

		/* set up the flags */

		alloc_flags |= PREALLOCATE;

		if (alloc_struct.fst_flags & F_ALLOCATECONTIG) {
			alloc_flags |= ALLOCATECONTIG;
		}

		if (alloc_struct.fst_flags & F_ALLOCATEALL) {
			alloc_flags |= ALLOCATEALL;
		}

		if (alloc_struct.fst_flags & F_ALLOCATEPERSIST) {
			alloc_flags |= ALLOCATEPERSIST;
		}

		/*
		 * Do any position mode specific stuff.  The only
		 * position mode  supported now is PEOFPOSMODE
		 */

		switch (alloc_struct.fst_posmode) {
		case F_PEOFPOSMODE:
			if (alloc_struct.fst_offset != 0) {
				error = EINVAL;
				goto outdrop;
			}

			alloc_flags |= ALLOCATEFROMPEOF;
			break;

		case F_VOLPOSMODE:
			if (alloc_struct.fst_offset <= 0) {
				error = EINVAL;
				goto outdrop;
			}

			alloc_flags |= ALLOCATEFROMVOL;
			break;

		default: {
			error = EINVAL;
			goto outdrop;
		}
		}
		if ((error = vnode_getwithref(vp)) == 0) {
			/*
			 * call allocate to get the space
			 */
			error = VNOP_ALLOCATE(vp, alloc_struct.fst_length, alloc_flags,
			    &alloc_struct.fst_bytesalloc, alloc_struct.fst_offset,
			    &context);
			(void)vnode_put(vp);

			error2 = copyout((caddr_t)&alloc_struct, argp, sizeof(alloc_struct));

			if (error == 0) {
				error = error2;
			}
		}
		goto outdrop;
	}
	case F_PUNCHHOLE: {
		fpunchhole_t args;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		/* need write permissions */
		if ((fp->f_flag & FWRITE) == 0) {
			error = EPERM;
			goto outdrop;
		}

		if ((error = copyin(argp, (caddr_t)&args, sizeof(args)))) {
			goto outdrop;
		}

		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}

#if CONFIG_MACF
		if ((error = mac_vnode_check_write(&context, fp->fp_glob->fg_cred, vp))) {
			(void)vnode_put(vp);
			goto outdrop;
		}
#endif

		error = VNOP_IOCTL(vp, F_PUNCHHOLE, (caddr_t)&args, 0, &context);
		(void)vnode_put(vp);

		goto outdrop;
	}
	case F_TRIM_ACTIVE_FILE: {
		ftrimactivefile_t args;

		if (priv_check_cred(kauth_cred_get(), PRIV_TRIM_ACTIVE_FILE, 0)) {
			error = EACCES;
			goto out;
		}

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		/* need write permissions */
		if ((fp->f_flag & FWRITE) == 0) {
			error = EPERM;
			goto outdrop;
		}

		if ((error = copyin(argp, (caddr_t)&args, sizeof(args)))) {
			goto outdrop;
		}

		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}

		error = VNOP_IOCTL(vp, F_TRIM_ACTIVE_FILE, (caddr_t)&args, 0, &context);
		(void)vnode_put(vp);

		goto outdrop;
	}
	case F_SPECULATIVE_READ: {
		fspecread_t args;
		off_t temp_length = 0;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = copyin(argp, (caddr_t)&args, sizeof(args)))) {
			goto outdrop;
		}

		/* Discard invalid offsets or lengths */
		if ((args.fsr_offset < 0) || (args.fsr_length < 0)) {
			error = EINVAL;
			goto outdrop;
		}

		/*
		 * Round the file offset down to a page-size boundary (or to 0).
		 * The filesystem will need to round the length up to the end of the page boundary
		 * or to the EOF of the file.
		 */
		uint64_t foff = (((uint64_t)args.fsr_offset) & ~((uint64_t)PAGE_MASK));
		uint64_t foff_delta = args.fsr_offset - foff;
		args.fsr_offset = (off_t) foff;

		/*
		 * Now add in the delta to the supplied length. Since we may have adjusted the
		 * offset, increase it by the amount that we adjusted.
		 */
		if (os_add_overflow(args.fsr_length, foff_delta, &args.fsr_length)) {
			error = EOVERFLOW;
			goto outdrop;
		}

		/*
		 * Make sure (fsr_offset + fsr_length) does not overflow.
		 */
		if (os_add_overflow(args.fsr_offset, args.fsr_length, &temp_length)) {
			error = EOVERFLOW;
			goto outdrop;
		}

		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}
		error = VNOP_IOCTL(vp, F_SPECULATIVE_READ, (caddr_t)&args, 0, &context);
		(void)vnode_put(vp);

		goto outdrop;
	}
	case F_ATTRIBUTION_TAG: {
		fattributiontag_t args;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = copyin(argp, (caddr_t)&args, sizeof(args)))) {
			goto outdrop;
		}

		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}

		error = VNOP_IOCTL(vp, F_ATTRIBUTION_TAG, (caddr_t)&args, 0, &context);
		(void)vnode_put(vp);

		if (error == 0) {
			error = copyout((caddr_t)&args, argp, sizeof(args));
		}

		goto outdrop;
	}
	case F_SETSIZE: {
		struct vnode_attr va;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		if ((fp->fp_glob->fg_flag & FWRITE) == 0) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		error = copyin(argp, (caddr_t)&offset, sizeof(off_t));
		if (error) {
			goto outdrop;
		}
		AUDIT_ARG(value64, offset);

		error = vnode_getwithref(vp);
		if (error) {
			goto outdrop;
		}

		VATTR_INIT(&va);
		VATTR_WANTED(&va, va_flags);

		error = vnode_getattr(vp, &va, vfs_context_current());
		if (error) {
			vnode_put(vp);
			goto outdrop;
		}

		/* Don't allow F_SETSIZE if the file has append-only flag set. */
		if (va.va_flags & APPEND) {
			error = EPERM;
			vnode_put(vp);
			goto outdrop;
		}

#if CONFIG_MACF
		error = mac_vnode_check_truncate(&context,
		    fp->fp_glob->fg_cred, vp);
		if (error) {
			(void)vnode_put(vp);
			goto outdrop;
		}
#endif
		/*
		 * Make sure that we are root.  Growing a file
		 * without zero filling the data is a security hole.
		 */
		if (!kauth_cred_issuser(kauth_cred_get())) {
			error = EACCES;
		} else {
			/*
			 * Require privilege to change file size without zerofill,
			 * else will change the file size and zerofill it.
			 */
			error = priv_check_cred(kauth_cred_get(), PRIV_VFS_SETSIZE, 0);
			if (error == 0) {
				error = vnode_setsize(vp, offset, IO_NOZEROFILL, &context);
			} else {
				error = vnode_setsize(vp, offset, 0, &context);
			}

#if CONFIG_MACF
			if (error == 0) {
				mac_vnode_notify_truncate(&context, fp->fp_glob->fg_cred, vp);
			}
#endif
		}

		(void)vnode_put(vp);
		goto outdrop;
	}

	case F_RDAHEAD:
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		if (uap->arg) {
			os_atomic_andnot(&fp->fp_glob->fg_flag, FNORDAHEAD, relaxed);
		} else {
			os_atomic_or(&fp->fp_glob->fg_flag, FNORDAHEAD, relaxed);
		}
		goto out;

	case F_NOCACHE:
	case F_NOCACHE_EXT:
		if ((fp->f_type != DTYPE_VNODE) || (cmd == F_NOCACHE_EXT &&
		    (vnode_vtype((struct vnode *)fp_get_data(fp)) != VREG))) {
			error = EBADF;
			goto out;
		}
		if (uap->arg) {
			os_atomic_or(&fp->fp_glob->fg_flag, FNOCACHE, relaxed);
			if (cmd == F_NOCACHE_EXT) {
				/*
				 * We're reusing the O_NOCTTY bit for this purpose as it is only
				 * used for open(2) and is mutually exclusive with a regular file.
				 */
				os_atomic_or(&fp->fp_glob->fg_flag, O_NOCTTY, relaxed);
			}
		} else {
			os_atomic_andnot(&fp->fp_glob->fg_flag, FNOCACHE | O_NOCTTY, relaxed);
		}
		goto out;

	case F_NODIRECT:
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		if (uap->arg) {
			os_atomic_or(&fp->fp_glob->fg_flag, FNODIRECT, relaxed);
		} else {
			os_atomic_andnot(&fp->fp_glob->fg_flag, FNODIRECT, relaxed);
		}
		goto out;

	case F_SINGLE_WRITER:
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		if (uap->arg) {
			os_atomic_or(&fp->fp_glob->fg_flag, FSINGLE_WRITER, relaxed);
		} else {
			os_atomic_andnot(&fp->fp_glob->fg_flag, FSINGLE_WRITER, relaxed);
		}
		goto out;

	case F_GLOBAL_NOCACHE:
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = vnode_getwithref(vp)) == 0) {
			*retval = vnode_isnocache(vp);

			if (uap->arg) {
				vnode_setnocache(vp);
			} else {
				vnode_clearnocache(vp);
			}

			(void)vnode_put(vp);
		}
		goto outdrop;

	case F_CHECK_OPENEVT:
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = vnode_getwithref(vp)) == 0) {
			*retval = vnode_is_openevt(vp);

			if (uap->arg) {
				vnode_set_openevt(vp);
			} else {
				vnode_clear_openevt(vp);
			}

			(void)vnode_put(vp);
		}
		goto outdrop;

	case F_RDADVISE: {
		struct radvisory ra_struct;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = copyin(argp, (caddr_t)&ra_struct, sizeof(ra_struct)))) {
			goto outdrop;
		}
		if (ra_struct.ra_offset < 0 || ra_struct.ra_count < 0) {
			error = EINVAL;
			goto outdrop;
		}
		if ((error = vnode_getwithref(vp)) == 0) {
			error = VNOP_IOCTL(vp, F_RDADVISE, (caddr_t)&ra_struct, 0, &context);

			(void)vnode_put(vp);
		}
		goto outdrop;
	}

	case F_FLUSH_DATA:

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = vnode_getwithref(vp)) == 0) {
			error = VNOP_FSYNC(vp, MNT_NOWAIT, &context);

			(void)vnode_put(vp);
		}
		goto outdrop;

	case F_LOG2PHYS:
	case F_LOG2PHYS_EXT: {
		struct log2phys l2p_struct = {};    /* structure for allocate command */
		int devBlockSize;

		off_t file_offset = 0;
		size_t a_size = 0;
		size_t run = 0;

		if (cmd == F_LOG2PHYS_EXT) {
			error = copyin(argp, (caddr_t)&l2p_struct, sizeof(l2p_struct));
			if (error) {
				goto out;
			}
			file_offset = l2p_struct.l2p_devoffset;
		} else {
			file_offset = fp->f_offset;
		}
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);
		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}
		error = VNOP_OFFTOBLK(vp, file_offset, &lbn);
		if (error) {
			(void)vnode_put(vp);
			goto outdrop;
		}
		error = VNOP_BLKTOOFF(vp, lbn, &offset);
		if (error) {
			(void)vnode_put(vp);
			goto outdrop;
		}
		devBlockSize = vfs_devblocksize(vnode_mount(vp));
		if (cmd == F_LOG2PHYS_EXT) {
			if (l2p_struct.l2p_contigbytes < 0) {
				vnode_put(vp);
				error = EINVAL;
				goto outdrop;
			}

			a_size = (size_t)MIN((uint64_t)l2p_struct.l2p_contigbytes, SIZE_MAX);
		} else {
			a_size = devBlockSize;
		}

		error = VNOP_BLOCKMAP(vp, offset, a_size, &bn, &run, NULL, 0, &context);

		(void)vnode_put(vp);

		if (!error) {
			l2p_struct.l2p_flags = 0;       /* for now */
			if (cmd == F_LOG2PHYS_EXT) {
				l2p_struct.l2p_contigbytes = run - (file_offset - offset);
			} else {
				l2p_struct.l2p_contigbytes = 0; /* for now */
			}

			/*
			 * The block number being -1 suggests that the file offset is not backed
			 * by any real blocks on-disk.  As a result, just let it be passed back up wholesale.
			 */
			if (bn == -1) {
				/* Don't multiply it by the block size */
				l2p_struct.l2p_devoffset = bn;
			} else {
				l2p_struct.l2p_devoffset = bn * devBlockSize;
				l2p_struct.l2p_devoffset += file_offset - offset;
			}
			error = copyout((caddr_t)&l2p_struct, argp, sizeof(l2p_struct));
		}
		goto outdrop;
	}
	case F_GETPATH:
	case F_GETPATH_NOFIRMLINK: {
		char *pathbufp;
		size_t pathlen;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		pathlen = MAXPATHLEN;
		pathbufp = zalloc(ZV_NAMEI);

		if ((error = vnode_getwithref(vp)) == 0) {
			error = vn_getpath_ext(vp, NULL, pathbufp,
			    &pathlen, cmd == F_GETPATH_NOFIRMLINK ?
			    VN_GETPATH_NO_FIRMLINK : 0);
			(void)vnode_put(vp);

			if (error == 0) {
				error = copyout((caddr_t)pathbufp, argp, pathlen);
			}
		}
		zfree(ZV_NAMEI, pathbufp);
		goto outdrop;
	}

	case F_PATHPKG_CHECK: {
		char *pathbufp;
		size_t pathlen;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		pathlen = MAXPATHLEN;
		pathbufp = zalloc(ZV_NAMEI);

		if ((error = copyinstr(argp, pathbufp, MAXPATHLEN, &pathlen)) == 0) {
			if ((error = vnode_getwithref(vp)) == 0) {
				AUDIT_ARG(text, pathbufp);
				error = vn_path_package_check(vp, pathbufp, (int)pathlen, retval);

				(void)vnode_put(vp);
			}
		}
		zfree(ZV_NAMEI, pathbufp);
		goto outdrop;
	}

	case F_CHKCLEAN:   // used by regression tests to see if all dirty pages got cleaned by fsync()
	case F_FULLFSYNC:  // fsync + flush the journal + DKIOCSYNCHRONIZE
	case F_BARRIERFSYNC:  // fsync + barrier
	case F_FREEZE_FS:  // freeze all other fs operations for the fs of this fd
	case F_THAW_FS: {  // thaw all frozen fs operations for the fs of this fd
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = vnode_getwithref(vp)) == 0) {
			if ((cmd == F_BARRIERFSYNC) &&
			    (vp->v_mount->mnt_supl_kern_flag & MNTK_SUPL_USE_FULLSYNC)) {
				cmd = F_FULLFSYNC;
			}
			error = VNOP_IOCTL(vp, cmd, (caddr_t)NULL, 0, &context);

			/*
			 * Promote F_BARRIERFSYNC to F_FULLFSYNC if the underlying
			 * filesystem doesn't support it.
			 */
			if ((error == ENOTTY || error == ENOTSUP || error == EINVAL) &&
			    (cmd == F_BARRIERFSYNC)) {
				os_atomic_or(&vp->v_mount->mnt_supl_kern_flag,
				    MNTK_SUPL_USE_FULLSYNC, relaxed);

				error = VNOP_IOCTL(vp, F_FULLFSYNC, (caddr_t)NULL, 0, &context);
			}

			(void)vnode_put(vp);
		}
		break;
	}

	/*
	 * SPI (private) for opening a file starting from a dir fd
	 */
	case F_OPENFROM: {
		/* Check if this isn't a valid file descriptor */
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);

		return sys_fcntl__OPENFROM(p, fd, cmd, uap->arg, fp, vp, retval);
	}

	/*
	 * SPI (private) for unlinking a file starting from a dir fd
	 */
	case F_UNLINKFROM: {
		user_addr_t pathname;

		/* Check if this isn't a valid file descriptor */
		if ((fp->f_type != DTYPE_VNODE) ||
		    (fp->f_flag & FREAD) == 0) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		/* Only valid for directories */
		if (vp->v_type != VDIR) {
			vnode_put(vp);
			error = ENOTDIR;
			goto outdrop;
		}

		/*
		 * Only entitled apps may use the credentials of the thread
		 * that opened the file descriptor.
		 * Non-entitled threads will use their own context.
		 */
		if (IOCurrentTaskHasEntitlement(ACCOUNT_OPENFROM_ENTITLEMENT)) {
			has_entitlement = 1;
		}

		/* Get flags, mode and pathname arguments. */
		if (IS_64BIT_PROCESS(p)) {
			pathname = (user_addr_t)argp;
		} else {
			pathname = CAST_USER_ADDR_T(argp);
		}

		/* Start the lookup relative to the file descriptor's vnode. */
		error = unlink1(has_entitlement ? &context : vfs_context_current(),
		    vp, pathname, UIO_USERSPACE, 0);

		vnode_put(vp);
		break;
	}

#if DEVELOPMENT || DEBUG
	case F_ADDSIGS_MAIN_BINARY:
		csblob_add_flags |= CS_BLOB_ADD_ALLOW_MAIN_BINARY;
		OS_FALLTHROUGH;
#endif
	case F_ADDSIGS:
	case F_ADDFILESIGS:
	case F_ADDFILESIGS_FOR_DYLD_SIM:
	case F_ADDFILESIGS_RETURN:
	case F_ADDFILESIGS_INFO:
	{
		struct cs_blob *blob = NULL;
		struct user_fsignatures fs;
		kern_return_t kr;
		vm_offset_t kernel_blob_addr;
		vm_size_t kernel_blob_size;
		int blob_add_flags = 0;
		const size_t sizeof_fs = (cmd == F_ADDFILESIGS_INFO ?
		    offsetof(struct user_fsignatures, fs_cdhash /* first output element */) :
		    offsetof(struct user_fsignatures, fs_fsignatures_size /* compat */));

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if (cmd == F_ADDFILESIGS_FOR_DYLD_SIM) {
			blob_add_flags |= MAC_VNODE_CHECK_DYLD_SIM;
			if ((proc_getcsflags(p) & CS_KILL) == 0) {
				proc_lock(p);
				proc_csflags_set(p, CS_KILL);
				proc_unlock(p);
			}
		}

		error = vnode_getwithref(vp);
		if (error) {
			goto outdrop;
		}

		if (IS_64BIT_PROCESS(p)) {
			error = copyin(argp, &fs, sizeof_fs);
		} else {
			if (cmd == F_ADDFILESIGS_INFO) {
				error = EINVAL;
				vnode_put(vp);
				goto outdrop;
			}

			struct user32_fsignatures fs32;

			error = copyin(argp, &fs32, sizeof(fs32));
			fs.fs_file_start = fs32.fs_file_start;
			fs.fs_blob_start = CAST_USER_ADDR_T(fs32.fs_blob_start);
			fs.fs_blob_size = fs32.fs_blob_size;
		}

		if (error) {
			vnode_put(vp);
			goto outdrop;
		}

		/*
		 * First check if we have something loaded a this offset
		 */
		blob = ubc_cs_blob_get(vp, CPU_TYPE_ANY, CPU_SUBTYPE_ANY, fs.fs_file_start);
		if (blob != NULL) {
			/* If this is for dyld_sim revalidate the blob */
			if (cmd == F_ADDFILESIGS_FOR_DYLD_SIM) {
				error = ubc_cs_blob_revalidate(vp, blob, NULL, blob_add_flags, proc_platform(p));
				if (error) {
					blob = NULL;
					if (error != EAGAIN) {
						vnode_put(vp);
						goto outdrop;
					}
				}
			}
		}

		if (blob == NULL) {
			/*
			 * An arbitrary limit, to prevent someone from mapping in a 20GB blob.  This should cover
			 * our use cases for the immediate future, but note that at the time of this commit, some
			 * platforms are nearing 2MB blob sizes (with a prior soft limit of 2.5MB).
			 *
			 * We should consider how we can manage this more effectively; the above means that some
			 * platforms are using megabytes of memory for signing data; it merely hasn't crossed the
			 * threshold considered ridiculous at the time of this change.
			 */
#define CS_MAX_BLOB_SIZE (40ULL * 1024ULL * 1024ULL)
			if (fs.fs_blob_size > CS_MAX_BLOB_SIZE) {
				error = E2BIG;
				vnode_put(vp);
				goto outdrop;
			}

			kernel_blob_size = CAST_DOWN(vm_size_t, fs.fs_blob_size);
			kr = ubc_cs_blob_allocate(&kernel_blob_addr, &kernel_blob_size);
			if (kr != KERN_SUCCESS || kernel_blob_size < fs.fs_blob_size) {
				error = ENOMEM;
				vnode_put(vp);
				goto outdrop;
			}

			if (cmd == F_ADDSIGS || cmd == F_ADDSIGS_MAIN_BINARY) {
				error = copyin(fs.fs_blob_start,
				    (void *) kernel_blob_addr,
				    fs.fs_blob_size);
			} else { /* F_ADDFILESIGS || F_ADDFILESIGS_RETURN || F_ADDFILESIGS_FOR_DYLD_SIM || F_ADDFILESIGS_INFO */
				int resid;

				error = vn_rdwr(UIO_READ,
				    vp,
				    (caddr_t) kernel_blob_addr,
				    (int)kernel_blob_size,
				    fs.fs_file_start + fs.fs_blob_start,
				    UIO_SYSSPACE,
				    0,
				    kauth_cred_get(),
				    &resid,
				    p);
				if ((error == 0) && resid) {
					/* kernel_blob_size rounded to a page size, but signature may be at end of file */
					memset((void *)(kernel_blob_addr + (kernel_blob_size - resid)), 0x0, resid);
				}
			}

			if (error) {
				ubc_cs_blob_deallocate(kernel_blob_addr,
				    kernel_blob_size);
				vnode_put(vp);
				goto outdrop;
			}

			blob = NULL;
			error = ubc_cs_blob_add(vp,
			    proc_platform(p),
			    CPU_TYPE_ANY,                       /* not for a specific architecture */
			    CPU_SUBTYPE_ANY,
			    fs.fs_file_start,
			    &kernel_blob_addr,
			    kernel_blob_size,
			    NULL,
			    blob_add_flags,
			    &blob,
			    csblob_add_flags);

			/* ubc_blob_add() has consumed "kernel_blob_addr" if it is zeroed */
			if (error) {
				if (kernel_blob_addr) {
					ubc_cs_blob_deallocate(kernel_blob_addr,
					    kernel_blob_size);
				}
				vnode_put(vp);
				goto outdrop;
			} else {
#if CHECK_CS_VALIDATION_BITMAP
				ubc_cs_validation_bitmap_allocate( vp );
#endif
			}
		}

		if (cmd == F_ADDFILESIGS_RETURN || cmd == F_ADDFILESIGS_FOR_DYLD_SIM ||
		    cmd == F_ADDFILESIGS_INFO) {
			/*
			 * The first element of the structure is a
			 * off_t that happen to have the same size for
			 * all archs. Lets overwrite that.
			 */
			off_t end_offset = 0;
			if (blob) {
				end_offset = blob->csb_end_offset;
			}
			error = copyout(&end_offset, argp, sizeof(end_offset));

			if (error) {
				vnode_put(vp);
				goto outdrop;
			}
		}

		if (cmd == F_ADDFILESIGS_INFO) {
			/* Return information. What we copy out depends on the size of the
			 * passed in structure, to keep binary compatibility. */

			if (fs.fs_fsignatures_size >= sizeof(struct user_fsignatures)) {
				// enough room for fs_cdhash[20]+fs_hash_type

				if (blob != NULL) {
					error = copyout(blob->csb_cdhash,
					    (vm_address_t)argp + offsetof(struct user_fsignatures, fs_cdhash),
					    USER_FSIGNATURES_CDHASH_LEN);
					if (error) {
						vnode_put(vp);
						goto outdrop;
					}
					int hashtype = cs_hash_type(blob->csb_hashtype);
					error = copyout(&hashtype,
					    (vm_address_t)argp + offsetof(struct user_fsignatures, fs_hash_type),
					    sizeof(int));
					if (error) {
						vnode_put(vp);
						goto outdrop;
					}
				}
			}
		}

		(void) vnode_put(vp);
		break;
	}
#if CONFIG_SUPPLEMENTAL_SIGNATURES
	case F_ADDFILESUPPL:
	{
		struct vnode *ivp;
		struct cs_blob *blob = NULL;
		struct user_fsupplement fs;
		int orig_fd;
		struct fileproc* orig_fp = NULL;
		kern_return_t kr;
		vm_offset_t kernel_blob_addr;
		vm_size_t kernel_blob_size;

		if (!IS_64BIT_PROCESS(p)) {
			error = EINVAL;
			goto out; // drop fp and unlock fds
		}

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		error = copyin(argp, &fs, sizeof(fs));
		if (error) {
			goto out;
		}

		orig_fd = fs.fs_orig_fd;
		if ((error = fp_lookup(p, orig_fd, &orig_fp, 1))) {
			printf("CODE SIGNING: Failed to find original file for supplemental signature attachment\n");
			goto out;
		}

		if (orig_fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			fp_drop(p, orig_fd, orig_fp, 1);
			goto out;
		}

		ivp = (struct vnode *)fp_get_data(orig_fp);

		vp = (struct vnode *)fp_get_data(fp);

		proc_fdunlock(p);

		error = vnode_getwithref(ivp);
		if (error) {
			fp_drop(p, orig_fd, orig_fp, 0);
			goto outdrop; //drop fp
		}

		error = vnode_getwithref(vp);
		if (error) {
			vnode_put(ivp);
			fp_drop(p, orig_fd, orig_fp, 0);
			goto outdrop;
		}

		if (fs.fs_blob_size > CS_MAX_BLOB_SIZE) {
			error = E2BIG;
			goto dropboth; // drop iocounts on vp and ivp, drop orig_fp then drop fp via outdrop
		}

		kernel_blob_size = CAST_DOWN(vm_size_t, fs.fs_blob_size);
		kr = ubc_cs_blob_allocate(&kernel_blob_addr, &kernel_blob_size);
		if (kr != KERN_SUCCESS) {
			error = ENOMEM;
			goto dropboth;
		}

		int resid;
		error = vn_rdwr(UIO_READ, vp,
		    (caddr_t)kernel_blob_addr, (int)kernel_blob_size,
		    fs.fs_file_start + fs.fs_blob_start,
		    UIO_SYSSPACE, 0,
		    kauth_cred_get(), &resid, p);
		if ((error == 0) && resid) {
			/* kernel_blob_size rounded to a page size, but signature may be at end of file */
			memset((void *)(kernel_blob_addr + (kernel_blob_size - resid)), 0x0, resid);
		}

		if (error) {
			ubc_cs_blob_deallocate(kernel_blob_addr,
			    kernel_blob_size);
			goto dropboth;
		}

		error = ubc_cs_blob_add_supplement(vp, ivp, fs.fs_file_start,
		    &kernel_blob_addr, kernel_blob_size, &blob);

		/* ubc_blob_add_supplement() has consumed kernel_blob_addr if it is zeroed */
		if (error) {
			if (kernel_blob_addr) {
				ubc_cs_blob_deallocate(kernel_blob_addr,
				    kernel_blob_size);
			}
			goto dropboth;
		}
		vnode_put(ivp);
		vnode_put(vp);
		fp_drop(p, orig_fd, orig_fp, 0);
		break;

dropboth:
		vnode_put(ivp);
		vnode_put(vp);
		fp_drop(p, orig_fd, orig_fp, 0);
		goto outdrop;
	}
#endif
	case F_GETCODEDIR:
	case F_FINDSIGS: {
		error = ENOTSUP;
		goto out;
	}
	case F_CHECK_LV: {
		struct fileglob *fg;
		fchecklv_t lv = {};

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		fg = fp->fp_glob;
		proc_fdunlock(p);

		if (IS_64BIT_PROCESS(p)) {
			error = copyin(argp, &lv, sizeof(lv));
		} else {
			struct user32_fchecklv lv32 = {};

			error = copyin(argp, &lv32, sizeof(lv32));
			lv.lv_file_start = lv32.lv_file_start;
			lv.lv_error_message = (void *)(uintptr_t)lv32.lv_error_message;
			lv.lv_error_message_size = lv32.lv_error_message_size;
		}
		if (error) {
			goto outdrop;
		}

#if CONFIG_MACF
		error = mac_file_check_library_validation(p, fg, lv.lv_file_start,
		    (user_long_t)lv.lv_error_message, lv.lv_error_message_size);
#endif

		break;
	}
	case F_GETSIGSINFO: {
		struct cs_blob *blob = NULL;
		fgetsigsinfo_t sigsinfo = {};

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		error = vnode_getwithref(vp);
		if (error) {
			goto outdrop;
		}

		error = copyin(argp, &sigsinfo, sizeof(sigsinfo));
		if (error) {
			vnode_put(vp);
			goto outdrop;
		}

		blob = ubc_cs_blob_get(vp, CPU_TYPE_ANY, CPU_SUBTYPE_ANY, sigsinfo.fg_file_start);
		if (blob == NULL) {
			error = ENOENT;
			vnode_put(vp);
			goto outdrop;
		}
		switch (sigsinfo.fg_info_request) {
		case GETSIGSINFO_PLATFORM_BINARY:
			sigsinfo.fg_sig_is_platform = blob->csb_platform_binary;
			error = copyout(&sigsinfo.fg_sig_is_platform,
			    (vm_address_t)argp + offsetof(struct fgetsigsinfo, fg_sig_is_platform),
			    sizeof(sigsinfo.fg_sig_is_platform));
			if (error) {
				vnode_put(vp);
				goto outdrop;
			}
			break;
		default:
			error = EINVAL;
			vnode_put(vp);
			goto outdrop;
		}
		vnode_put(vp);
		break;
	}
#if CONFIG_PROTECT
	case F_GETPROTECTIONCLASS: {
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);

		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		struct vnode_attr va;

		VATTR_INIT(&va);
		VATTR_WANTED(&va, va_dataprotect_class);
		error = VNOP_GETATTR(vp, &va, &context);
		if (!error) {
			if (VATTR_IS_SUPPORTED(&va, va_dataprotect_class)) {
				*retval = va.va_dataprotect_class;
			} else {
				error = ENOTSUP;
			}
		}

		vnode_put(vp);
		break;
	}

	case F_SETPROTECTIONCLASS: {
		/* tmp must be a valid PROTECTION_CLASS_* */
		tmp = CAST_DOWN_EXPLICIT(uint32_t, uap->arg);

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);

		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		/* Only go forward if you have write access */
		vfs_context_t ctx = vfs_context_current();
		if (vnode_authorize(vp, NULLVP, (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), ctx) != 0) {
			vnode_put(vp);
			error = EBADF;
			goto outdrop;
		}

		struct vnode_attr va;

		VATTR_INIT(&va);
		VATTR_SET(&va, va_dataprotect_class, tmp);

		error = VNOP_SETATTR(vp, &va, ctx);

		vnode_put(vp);
		break;
	}

	case F_TRANSCODEKEY: {
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		cp_key_t k = {
			.len = CP_MAX_WRAPPEDKEYSIZE,
		};

		k.key = kalloc_data(CP_MAX_WRAPPEDKEYSIZE, Z_WAITOK | Z_ZERO);
		if (k.key == NULL) {
			error = ENOMEM;
		} else {
			error = VNOP_IOCTL(vp, F_TRANSCODEKEY, (caddr_t)&k, 1, &context);
		}

		vnode_put(vp);

		if (error == 0) {
			error = copyout(k.key, argp, k.len);
			*retval = k.len;
		}
		kfree_data(k.key, CP_MAX_WRAPPEDKEYSIZE);

		break;
	}

	case F_GETPROTECTIONLEVEL:  {
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode*)fp_get_data(fp);
		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		error = VNOP_IOCTL(vp, F_GETPROTECTIONLEVEL, (caddr_t)retval, 0, &context);

		vnode_put(vp);
		break;
	}

	case F_GETDEFAULTPROTLEVEL:  {
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode*)fp_get_data(fp);
		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		/*
		 * if cp_get_major_vers fails, error will be set to proper errno
		 * and cp_version will still be 0.
		 */

		error = VNOP_IOCTL(vp, F_GETDEFAULTPROTLEVEL, (caddr_t)retval, 0, &context);

		vnode_put(vp);
		break;
	}

#endif /* CONFIG_PROTECT */

	case F_MOVEDATAEXTENTS: {
		struct fileproc *fp2 = NULL;
		struct vnode *src_vp = NULLVP;
		struct vnode *dst_vp = NULLVP;
		/* We need to grab the 2nd FD out of the arguments before moving on. */
		int fd2 = CAST_DOWN_EXPLICIT(int32_t, uap->arg);

		error = priv_check_cred(kauth_cred_get(), PRIV_VFS_MOVE_DATA_EXTENTS, 0);
		if (error) {
			goto out;
		}

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		/*
		 * For now, special case HFS+ and APFS only, since this
		 * is SPI.
		 */
		src_vp = (struct vnode *)fp_get_data(fp);
		if (src_vp->v_tag != VT_HFS && src_vp->v_tag != VT_APFS) {
			error = ENOTSUP;
			goto out;
		}

		/*
		 * Get the references before we start acquiring iocounts on the vnodes,
		 * while we still hold the proc fd lock
		 */
		if ((error = fp_lookup(p, fd2, &fp2, 1))) {
			error = EBADF;
			goto out;
		}
		if (fp2->f_type != DTYPE_VNODE) {
			fp_drop(p, fd2, fp2, 1);
			error = EBADF;
			goto out;
		}
		dst_vp = (struct vnode *)fp_get_data(fp2);
		if (dst_vp->v_tag != VT_HFS && dst_vp->v_tag != VT_APFS) {
			fp_drop(p, fd2, fp2, 1);
			error = ENOTSUP;
			goto out;
		}

#if CONFIG_MACF
		/* Re-do MAC checks against the new FD, pass in a fake argument */
		error = mac_file_check_fcntl(kauth_cred_get(), fp2->fp_glob, cmd, 0);
		if (error) {
			fp_drop(p, fd2, fp2, 1);
			goto out;
		}
#endif
		/* Audit the 2nd FD */
		AUDIT_ARG(fd, fd2);

		proc_fdunlock(p);

		if (vnode_getwithref(src_vp)) {
			fp_drop(p, fd2, fp2, 0);
			error = ENOENT;
			goto outdrop;
		}
		if (vnode_getwithref(dst_vp)) {
			vnode_put(src_vp);
			fp_drop(p, fd2, fp2, 0);
			error = ENOENT;
			goto outdrop;
		}

		/*
		 * Basic asserts; validate they are not the same and that
		 * both live on the same filesystem.
		 */
		if (dst_vp == src_vp) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EINVAL;
			goto outdrop;
		}

		if (dst_vp->v_mount != src_vp->v_mount) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EXDEV;
			goto outdrop;
		}

		/* Now we have a legit pair of FDs.  Go to work */

		/* Now check for write access to the target files */
		if (vnode_authorize(src_vp, NULLVP,
		    (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), &context) != 0) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EBADF;
			goto outdrop;
		}

		if (vnode_authorize(dst_vp, NULLVP,
		    (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), &context) != 0) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EBADF;
			goto outdrop;
		}

		/* Verify that both vps point to files and not directories */
		if (!vnode_isreg(src_vp) || !vnode_isreg(dst_vp)) {
			error = EINVAL;
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			goto outdrop;
		}

		/*
		 * The exchangedata syscall handler passes in 0 for the flags to VNOP_EXCHANGE.
		 * We'll pass in our special bit indicating that the new behavior is expected
		 */

		error = VNOP_EXCHANGE(src_vp, dst_vp, FSOPT_EXCHANGE_DATA_ONLY, &context);

		vnode_put(src_vp);
		vnode_put(dst_vp);
		fp_drop(p, fd2, fp2, 0);
		break;
	}

	case F_TRANSFEREXTENTS: {
		struct fileproc *fp2 = NULL;
		struct vnode *src_vp = NULLVP;
		struct vnode *dst_vp = NULLVP;

		/* Get 2nd FD out of the arguments. */
		int fd2 = CAST_DOWN_EXPLICIT(int, uap->arg);
		if (fd2 < 0) {
			error = EINVAL;
			goto out;
		}

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		/*
		 * Only allow this for APFS
		 */
		src_vp = (struct vnode *)fp_get_data(fp);
		if (src_vp->v_tag != VT_APFS) {
			error = ENOTSUP;
			goto out;
		}

		/*
		 * Get the references before we start acquiring iocounts on the vnodes,
		 * while we still hold the proc fd lock
		 */
		if ((error = fp_lookup(p, fd2, &fp2, 1))) {
			error = EBADF;
			goto out;
		}
		if (fp2->f_type != DTYPE_VNODE) {
			fp_drop(p, fd2, fp2, 1);
			error = EBADF;
			goto out;
		}
		dst_vp = (struct vnode *)fp_get_data(fp2);
		if (dst_vp->v_tag != VT_APFS) {
			fp_drop(p, fd2, fp2, 1);
			error = ENOTSUP;
			goto out;
		}

#if CONFIG_MACF
		/* Re-do MAC checks against the new FD, pass in a fake argument */
		error = mac_file_check_fcntl(kauth_cred_get(), fp2->fp_glob, cmd, 0);
		if (error) {
			fp_drop(p, fd2, fp2, 1);
			goto out;
		}
#endif
		/* Audit the 2nd FD */
		AUDIT_ARG(fd, fd2);

		proc_fdunlock(p);

		if (vnode_getwithref(src_vp)) {
			fp_drop(p, fd2, fp2, 0);
			error = ENOENT;
			goto outdrop;
		}
		if (vnode_getwithref(dst_vp)) {
			vnode_put(src_vp);
			fp_drop(p, fd2, fp2, 0);
			error = ENOENT;
			goto outdrop;
		}

		/*
		 * Validate they are not the same and that
		 * both live on the same filesystem.
		 */
		if (dst_vp == src_vp) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EINVAL;
			goto outdrop;
		}
		if (dst_vp->v_mount != src_vp->v_mount) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EXDEV;
			goto outdrop;
		}

		/* Verify that both vps point to files and not directories */
		if (!vnode_isreg(src_vp) || !vnode_isreg(dst_vp)) {
			error = EINVAL;
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			goto outdrop;
		}


		/*
		 * Okay, vps are legit. Check  access.  We'll require write access
		 * to both files.
		 */
		if (vnode_authorize(src_vp, NULLVP,
		    (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), &context) != 0) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EBADF;
			goto outdrop;
		}
		if (vnode_authorize(dst_vp, NULLVP,
		    (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), &context) != 0) {
			vnode_put(src_vp);
			vnode_put(dst_vp);
			fp_drop(p, fd2, fp2, 0);
			error = EBADF;
			goto outdrop;
		}

		/* Pass it on through to the fs */
		error = VNOP_IOCTL(src_vp, cmd, (caddr_t)dst_vp, 0, &context);

		vnode_put(src_vp);
		vnode_put(dst_vp);
		fp_drop(p, fd2, fp2, 0);
		break;
	}

	/*
	 * SPI for making a file compressed.
	 */
	case F_MAKECOMPRESSED: {
		uint32_t gcounter = CAST_DOWN_EXPLICIT(uint32_t, uap->arg);

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode*)fp_get_data(fp);
		proc_fdunlock(p);

		/* get the vnode */
		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		/* Is it a file? */
		if ((vnode_isreg(vp) == 0) && (vnode_islnk(vp) == 0)) {
			vnode_put(vp);
			error = EBADF;
			goto outdrop;
		}

		/* invoke ioctl to pass off to FS */
		/* Only go forward if you have write access */
		vfs_context_t ctx = vfs_context_current();
		if (vnode_authorize(vp, NULLVP, (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), ctx) != 0) {
			vnode_put(vp);
			error = EBADF;
			goto outdrop;
		}

		error = VNOP_IOCTL(vp, cmd, (caddr_t)&gcounter, 0, &context);

		vnode_put(vp);
		break;
	}

	/*
	 * SPI (private) for indicating to a filesystem that subsequent writes to
	 * the open FD will written to the Fastflow.
	 */
	case F_SET_GREEDY_MODE:
	/* intentionally drop through to the same handler as F_SETSTATIC.
	 * both fcntls should pass the argument and their selector into VNOP_IOCTL.
	 */

	/*
	 * SPI (private) for indicating to a filesystem that subsequent writes to
	 * the open FD will represent static content.
	 */
	case F_SETSTATICCONTENT: {
		caddr_t ioctl_arg = NULL;

		if (uap->arg) {
			ioctl_arg = (caddr_t) 1;
		}

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		error = vnode_getwithref(vp);
		if (error) {
			error = ENOENT;
			goto outdrop;
		}

		/* Only go forward if you have write access */
		vfs_context_t ctx = vfs_context_current();
		if (vnode_authorize(vp, NULLVP, (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), ctx) != 0) {
			vnode_put(vp);
			error = EBADF;
			goto outdrop;
		}

		error = VNOP_IOCTL(vp, cmd, ioctl_arg, 0, &context);
		(void)vnode_put(vp);

		break;
	}

	/*
	 * SPI (private) for indicating to the lower level storage driver that the
	 * subsequent writes should be of a particular IO type (burst, greedy, static),
	 * or other flavors that may be necessary.
	 */
	case F_SETIOTYPE: {
		caddr_t param_ptr;
		uint32_t param;

		if (uap->arg) {
			/* extract 32 bits of flags from userland */
			param_ptr = (caddr_t) uap->arg;
			param = (uint32_t) param_ptr;
		} else {
			/* If no argument is specified, error out */
			error = EINVAL;
			goto out;
		}

		/*
		 * Validate the different types of flags that can be specified:
		 * all of them are mutually exclusive for now.
		 */
		switch (param) {
		case F_IOTYPE_ISOCHRONOUS:
			break;

		default:
			error = EINVAL;
			goto out;
		}


		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		error = vnode_getwithref(vp);
		if (error) {
			error = ENOENT;
			goto outdrop;
		}

		/* Only go forward if you have write access */
		vfs_context_t ctx = vfs_context_current();
		if (vnode_authorize(vp, NULLVP, (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), ctx) != 0) {
			vnode_put(vp);
			error = EBADF;
			goto outdrop;
		}

		error = VNOP_IOCTL(vp, cmd, param_ptr, 0, &context);
		(void)vnode_put(vp);

		break;
	}

	/*
	 * Set the vnode pointed to by 'fd'
	 * and tag it as the (potentially future) backing store
	 * for another filesystem
	 */
	case F_SETBACKINGSTORE: {
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);

		if (vp->v_tag != VT_HFS) {
			error = EINVAL;
			goto out;
		}
		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		/* only proceed if you have write access */
		vfs_context_t ctx = vfs_context_current();
		if (vnode_authorize(vp, NULLVP, (KAUTH_VNODE_ACCESS | KAUTH_VNODE_WRITE_DATA), ctx) != 0) {
			vnode_put(vp);
			error = EBADF;
			goto outdrop;
		}


		/* If arg != 0, set, otherwise unset */
		if (uap->arg) {
			error = VNOP_IOCTL(vp, cmd, (caddr_t)1, 0, &context);
		} else {
			error = VNOP_IOCTL(vp, cmd, (caddr_t)NULL, 0, &context);
		}

		vnode_put(vp);
		break;
	}

	/*
	 * like F_GETPATH, but special semantics for
	 * the mobile time machine handler.
	 */
	case F_GETPATH_MTMINFO: {
		char *pathbufp;
		int pathlen;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		pathlen = MAXPATHLEN;
		pathbufp = zalloc(ZV_NAMEI);

		if ((error = vnode_getwithref(vp)) == 0) {
			int backingstore = 0;

			/* Check for error from vn_getpath before moving on */
			if ((error = vn_getpath(vp, pathbufp, &pathlen)) == 0) {
				if (vp->v_tag == VT_HFS) {
					error = VNOP_IOCTL(vp, cmd, (caddr_t) &backingstore, 0, &context);
				}
				(void)vnode_put(vp);

				if (error == 0) {
					error = copyout((caddr_t)pathbufp, argp, pathlen);
				}
				if (error == 0) {
					/*
					 * If the copyout was successful, now check to ensure
					 * that this vnode is not a BACKINGSTORE vnode.  mtmd
					 * wants the path regardless.
					 */
					if (backingstore) {
						error = EBUSY;
					}
				}
			} else {
				(void)vnode_put(vp);
			}
		}

		zfree(ZV_NAMEI, pathbufp);
		goto outdrop;
	}

	case F_RECYCLE: {
#if !DEBUG && !DEVELOPMENT
		bool allowed = false;

		//
		// non-debug and non-development kernels have restrictions
		// on who can all this fcntl.  the process has to be marked
		// with the dataless-manipulator entitlement and either the
		// process or thread have to be marked rapid-aging.
		//
		if (!vfs_context_is_dataless_manipulator(&context)) {
			error = EPERM;
			goto out;
		}

		proc_t proc = vfs_context_proc(&context);
		if (proc && (proc->p_lflag & P_LRAGE_VNODES)) {
			allowed = true;
		} else {
			thread_t thr = vfs_context_thread(&context);
			if (thr) {
				struct uthread *ut = get_bsdthread_info(thr);

				if (ut && (ut->uu_flag & UT_RAGE_VNODES)) {
					allowed = true;
				}
			}
		}
		if (!allowed) {
			error = EPERM;
			goto out;
		}
#endif

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		vnode_recycle(vp);
		break;
	}

#if CONFIG_FILE_LEASES
	case F_SETLEASE: {
		struct fileglob *fg;
		int fl_type;
		int expcounts;

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		fg = fp->fp_glob;;
		proc_fdunlock(p);

		/*
		 * In order to allow a process to avoid breaking
		 * its own leases, the expected open count needs
		 * to be provided to F_SETLEASE when placing write lease.
		 * Similarly, in order to allow a process to place a read lease
		 * after opening the file multiple times in RW mode, the expected
		 * write count needs to be provided to F_SETLEASE when placing a
		 * read lease.
		 *
		 * We use the upper 30 bits of the integer argument (way more than
		 * enough) as the expected open/write count.
		 *
		 * If the caller passed 0 for the expected open count,
		 * assume 1.
		 */
		fl_type = CAST_DOWN_EXPLICIT(int, uap->arg);
		expcounts = (unsigned int)fl_type >> 2;
		fl_type &= 3;

		if (fl_type == F_WRLCK && expcounts == 0) {
			expcounts = 1;
		}

		AUDIT_ARG(value32, fl_type);

		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}

		/*
		 * Only support for regular file/dir mounted on local-based filesystem.
		 */
		if ((vnode_vtype(vp) != VREG && vnode_vtype(vp) != VDIR) ||
		    !(vfs_flags(vnode_mount(vp)) & MNT_LOCAL)) {
			error = EBADF;
			vnode_put(vp);
			goto outdrop;
		}

		/* For directory, we only support read lease. */
		if (vnode_vtype(vp) == VDIR && fl_type == F_WRLCK) {
			error = ENOTSUP;
			vnode_put(vp);
			goto outdrop;
		}

		switch (fl_type) {
		case F_RDLCK:
		case F_WRLCK:
		case F_UNLCK:
			error = vnode_setlease(vp, fg, fl_type, expcounts,
			    vfs_context_current());
			break;
		default:
			error = EINVAL;
			break;
		}

		vnode_put(vp);
		goto outdrop;
	}

	case F_GETLEASE: {
		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = vnode_getwithref(vp))) {
			goto outdrop;
		}

		if ((vnode_vtype(vp) != VREG && vnode_vtype(vp) != VDIR) ||
		    !(vfs_flags(vnode_mount(vp)) & MNT_LOCAL)) {
			error = EBADF;
			vnode_put(vp);
			goto outdrop;
		}

		error = 0;
		*retval = vnode_getlease(vp);
		vnode_put(vp);
		goto outdrop;
	}
#endif /* CONFIG_FILE_LEASES */

	/* SPI (private) for asserting background access to a file */
	case F_ASSERT_BG_ACCESS:
	/* SPI (private) for releasing background access to a file */
	case F_RELEASE_BG_ACCESS: {
		/*
		 * Check if the process is platform code, which means
		 * that it is considered part of the Operating System.
		 */
		if (!csproc_get_platform_binary(p)) {
			error = EPERM;
			goto out;
		}

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}

		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if (vnode_getwithref(vp)) {
			error = ENOENT;
			goto outdrop;
		}

		/* Verify that vp points to a file and not a directory */
		if (!vnode_isreg(vp)) {
			vnode_put(vp);
			error = EINVAL;
			goto outdrop;
		}

		/* Only proceed if you have read access */
		if (vnode_authorize(vp, NULLVP, (KAUTH_VNODE_ACCESS | KAUTH_VNODE_READ_DATA), &context) != 0) {
			vnode_put(vp);
			error = EBADF;
			goto outdrop;
		}

		if (cmd == F_ASSERT_BG_ACCESS) {
			fassertbgaccess_t args;

			if ((error = copyin(argp, (caddr_t)&args, sizeof(args)))) {
				vnode_put(vp);
				goto outdrop;
			}

			error = VNOP_IOCTL(vp, F_ASSERT_BG_ACCESS, (caddr_t)&args, 0, &context);
		} else {
			// cmd == F_RELEASE_BG_ACCESS
			error = VNOP_IOCTL(vp, F_RELEASE_BG_ACCESS, (caddr_t)NULL, 0, &context);
		}

		vnode_put(vp);

		goto outdrop;
	}

	default:
		/*
		 * This is an fcntl() that we d not recognize at this level;
		 * if this is a vnode, we send it down into the VNOP_IOCTL
		 * for this vnode; this can include special devices, and will
		 * effectively overload fcntl() to send ioctl()'s.
		 */
		if ((cmd & IOC_VOID) && (cmd & IOC_INOUT)) {
			error = EINVAL;
			goto out;
		}

		/*
		 * Catch any now-invalid fcntl() selectors.
		 * (When adding a selector to this list, it may be prudent
		 * to consider adding it to the list in fsctl_internal() as well.)
		 */
		switch (cmd) {
		case (int)APFSIOC_REVERT_TO_SNAPSHOT:
		case (int)FSIOC_FIOSEEKHOLE:
		case (int)FSIOC_FIOSEEKDATA:
		case (int)FSIOC_CAS_BSDFLAGS:
		case (int)FSIOC_KERNEL_ROOTAUTH:
		case (int)FSIOC_GRAFT_FS:
		case (int)FSIOC_UNGRAFT_FS:
		case (int)APFSIOC_IS_GRAFT_SUPPORTED:
		case (int)FSIOC_AUTH_FS:
		case HFS_GET_BOOT_INFO:
		case HFS_SET_BOOT_INFO:
		case FIOPINSWAP:
		case F_MARKDEPENDENCY:
		case TIOCREVOKE:
		case TIOCREVOKECLEAR:
			error = EINVAL;
			goto out;
		default:
			break;
		}

		if (fp->f_type != DTYPE_VNODE) {
			error = EBADF;
			goto out;
		}
		vp = (struct vnode *)fp_get_data(fp);
		proc_fdunlock(p);

		if ((error = vnode_getwithref(vp)) == 0) {
#define STK_PARAMS 128
			char stkbuf[STK_PARAMS] = {0};
			unsigned int size;
			caddr_t data, memp;
			/*
			 * For this to work properly, we have to copy in the
			 * ioctl() cmd argument if there is one; we must also
			 * check that a command parameter, if present, does
			 * not exceed the maximum command length dictated by
			 * the number of bits we have available in the command
			 * to represent a structure length.  Finally, we have
			 * to copy the results back out, if it is that type of
			 * ioctl().
			 */
			size = IOCPARM_LEN(cmd);
			if (size > IOCPARM_MAX) {
				(void)vnode_put(vp);
				error = EINVAL;
				break;
			}

			memp = NULL;
			if (size > sizeof(stkbuf)) {
				memp = (caddr_t)kalloc_data(size, Z_WAITOK);
				if (memp == 0) {
					(void)vnode_put(vp);
					error = ENOMEM;
					goto outdrop;
				}
				data = memp;
			} else {
				data = &stkbuf[0];
			}

			if (cmd & IOC_IN) {
				if (size) {
					/* structure */
					error = copyin(argp, data, size);
					if (error) {
						(void)vnode_put(vp);
						if (memp) {
							kfree_data(memp, size);
						}
						goto outdrop;
					}

					/* Bzero the section beyond that which was needed */
					if (size <= sizeof(stkbuf)) {
						bzero((((uint8_t*)data) + size), (sizeof(stkbuf) - size));
					}
				} else {
					/* int */
					if (is64bit) {
						*(user_addr_t *)data = argp;
					} else {
						*(uint32_t *)data = (uint32_t)argp;
					}
				};
			} else if ((cmd & IOC_OUT) && size) {
				/*
				 * Zero the buffer so the user always
				 * gets back something deterministic.
				 */
				bzero(data, size);
			} else if (cmd & IOC_VOID) {
				if (is64bit) {
					*(user_addr_t *)data = argp;
				} else {
					*(uint32_t *)data = (uint32_t)argp;
				}
			}

			error = VNOP_IOCTL(vp, cmd, CAST_DOWN(caddr_t, data), 0, &context);

			(void)vnode_put(vp);

			/* Copy any output data to user */
			if (error == 0 && (cmd & IOC_OUT) && size) {
				error = copyout(data, argp, size);
			}
			if (memp) {
				kfree_data(memp, size);
			}
		}
		break;
	}

outdrop:
	return sys_fcntl_outdrop(p, fd, fp, vp, error);

out:
	return sys_fcntl_out(p, fd, fp, error);
}


/*
 * sys_close
 *
 * Description:	The implementation of the close(2) system call
 *
 * Parameters:	p			Process in whose per process file table
 *					the close is to occur
 *		uap->fd			fd to be closed
 *		retval			<unused>
 *
 * Returns:	0			Success
 *	fp_lookup:EBADF			Bad file descriptor
 *      fp_guard_exception:???          Guarded file descriptor
 *	close_internal:EBADF
 *	close_internal:???              Anything returnable by a per-fileops
 *					close function
 */
int
sys_close(proc_t p, struct close_args *uap, __unused int32_t *retval)
{
	kauth_cred_t p_cred = current_cached_proc_cred(p);

	__pthread_testcancel(1);
	return close_nocancel(p, p_cred, uap->fd);
}

int
sys_close_nocancel(proc_t p, struct close_nocancel_args *uap, __unused int32_t *retval)
{
	kauth_cred_t p_cred = current_cached_proc_cred(p);

	return close_nocancel(p, p_cred, uap->fd);
}

int
close_nocancel(proc_t p, kauth_cred_t p_cred, int fd)
{
	struct fileproc *fp;

	AUDIT_SYSCLOSE(p, fd);

	proc_fdlock(p);
	if ((fp = fp_get_noref_locked(p, fd)) == NULL) {
		proc_fdunlock(p);
		return EBADF;
	}

	if (fp_isguarded(fp, GUARD_CLOSE)) {
		int error = fp_guard_exception(p, fd, fp, kGUARD_EXC_CLOSE);
		proc_fdunlock(p);
		return error;
	}

	return fp_close_and_unlock(p, p_cred, fd, fp, 0);
}


/*
 * fstat
 *
 * Description:	Return status information about a file descriptor.
 *
 * Parameters:	p				The process doing the fstat
 *		fd				The fd to stat
 *		ub				The user stat buffer
 *		xsecurity			The user extended security
 *						buffer, or 0 if none
 *		xsecurity_size			The size of xsecurity, or 0
 *						if no xsecurity
 *		isstat64			Flag to indicate 64 bit version
 *						for inode size, etc.
 *
 * Returns:	0				Success
 *		EBADF
 *		EFAULT
 *	fp_lookup:EBADF				Bad file descriptor
 *	vnode_getwithref:???
 *	copyout:EFAULT
 *	vnode_getwithref:???
 *	vn_stat:???
 *	soo_stat:???
 *	pipe_stat:???
 *	pshm_stat:???
 *	kqueue_stat:???
 *
 * Notes:	Internal implementation for all other fstat() related
 *		functions
 *
 *		XXX switch on node type is bogus; need a stat in struct
 *		XXX fileops instead.
 */
static int
fstat(proc_t p, int fd, user_addr_t ub, user_addr_t xsecurity,
    user_addr_t xsecurity_size, int isstat64)
{
	struct fileproc *fp;
	union {
		struct stat sb;
		struct stat64 sb64;
	} source;
	union {
		struct user64_stat user64_sb;
		struct user32_stat user32_sb;
		struct user64_stat64 user64_sb64;
		struct user32_stat64 user32_sb64;
	} dest;
	int error, my_size;
	file_type_t type;
	caddr_t data;
	kauth_filesec_t fsec;
	user_size_t xsecurity_bufsize;
	vfs_context_t ctx = vfs_context_current();
	void * sbptr;


	AUDIT_ARG(fd, fd);

	if ((error = fp_lookup(p, fd, &fp, 0)) != 0) {
		return error;
	}
	type = fp->f_type;
	data = (caddr_t)fp_get_data(fp);
	fsec = KAUTH_FILESEC_NONE;

	sbptr = (void *)&source;

	switch (type) {
	case DTYPE_VNODE:
		if ((error = vnode_getwithref((vnode_t)data)) == 0) {
			/*
			 * If the caller has the file open, and is not
			 * requesting extended security information, we are
			 * going to let them get the basic stat information.
			 */
			if (xsecurity == USER_ADDR_NULL) {
				error = vn_stat_noauth((vnode_t)data, sbptr, NULL, isstat64, 0, ctx,
				    fp->fp_glob->fg_cred);
			} else {
				error = vn_stat((vnode_t)data, sbptr, &fsec, isstat64, 0, ctx);
			}

			AUDIT_ARG(vnpath, (struct vnode *)data, ARG_VNODE1);
			(void)vnode_put((vnode_t)data);
		}
		break;

#if SOCKETS
	case DTYPE_SOCKET:
		error = soo_stat((struct socket *)data, sbptr, isstat64);
		break;
#endif /* SOCKETS */

	case DTYPE_PIPE:
		error = pipe_stat((void *)data, sbptr, isstat64);
		break;

	case DTYPE_PSXSHM:
		error = pshm_stat((void *)data, sbptr, isstat64);
		break;

	case DTYPE_KQUEUE:
		error = kqueue_stat((void *)data, sbptr, isstat64, p);
		break;

	default:
		error = EBADF;
		goto out;
	}
	if (error == 0) {
		caddr_t sbp;

		if (isstat64 != 0) {
			source.sb64.st_lspare = 0;
			source.sb64.st_qspare[0] = 0LL;
			source.sb64.st_qspare[1] = 0LL;

			if (IS_64BIT_PROCESS(p)) {
				munge_user64_stat64(&source.sb64, &dest.user64_sb64);
				my_size = sizeof(dest.user64_sb64);
				sbp = (caddr_t)&dest.user64_sb64;
			} else {
				munge_user32_stat64(&source.sb64, &dest.user32_sb64);
				my_size = sizeof(dest.user32_sb64);
				sbp = (caddr_t)&dest.user32_sb64;
			}
		} else {
			source.sb.st_lspare = 0;
			source.sb.st_qspare[0] = 0LL;
			source.sb.st_qspare[1] = 0LL;
			if (IS_64BIT_PROCESS(p)) {
				munge_user64_stat(&source.sb, &dest.user64_sb);
				my_size = sizeof(dest.user64_sb);
				sbp = (caddr_t)&dest.user64_sb;
			} else {
				munge_user32_stat(&source.sb, &dest.user32_sb);
				my_size = sizeof(dest.user32_sb);
				sbp = (caddr_t)&dest.user32_sb;
			}
		}

		error = copyout(sbp, ub, my_size);
	}

	/* caller wants extended security information? */
	if (xsecurity != USER_ADDR_NULL) {
		/* did we get any? */
		if (fsec == KAUTH_FILESEC_NONE) {
			if (susize(xsecurity_size, 0) != 0) {
				error = EFAULT;
				goto out;
			}
		} else {
			/* find the user buffer size */
			xsecurity_bufsize = fusize(xsecurity_size);

			/* copy out the actual data size */
			if (susize(xsecurity_size, KAUTH_FILESEC_COPYSIZE(fsec)) != 0) {
				error = EFAULT;
				goto out;
			}

			/* if the caller supplied enough room, copy out to it */
			if (xsecurity_bufsize >= KAUTH_FILESEC_COPYSIZE(fsec)) {
				error = copyout(fsec, xsecurity, KAUTH_FILESEC_COPYSIZE(fsec));
			}
		}
	}
out:
	fp_drop(p, fd, fp, 0);
	if (fsec != NULL) {
		kauth_filesec_free(fsec);
	}
	return error;
}


/*
 * sys_fstat_extended
 *
 * Description:	Extended version of fstat supporting returning extended
 *		security information
 *
 * Parameters:	p				The process doing the fstat
 *		uap->fd				The fd to stat
 *		uap->ub				The user stat buffer
 *		uap->xsecurity			The user extended security
 *						buffer, or 0 if none
 *		uap->xsecurity_size		The size of xsecurity, or 0
 *
 * Returns:	0				Success
 *		!0				Errno (see fstat)
 */
int
sys_fstat_extended(proc_t p, struct fstat_extended_args *uap, __unused int32_t *retval)
{
	return fstat(p, uap->fd, uap->ub, uap->xsecurity, uap->xsecurity_size, 0);
}


/*
 * sys_fstat
 *
 * Description:	Get file status for the file associated with fd
 *
 * Parameters:	p				The process doing the fstat
 *		uap->fd				The fd to stat
 *		uap->ub				The user stat buffer
 *
 * Returns:	0				Success
 *		!0				Errno (see fstat)
 */
int
sys_fstat(proc_t p, struct fstat_args *uap, __unused int32_t *retval)
{
	return fstat(p, uap->fd, uap->ub, 0, 0, 0);
}


/*
 * sys_fstat64_extended
 *
 * Description:	Extended version of fstat64 supporting returning extended
 *		security information
 *
 * Parameters:	p				The process doing the fstat
 *		uap->fd				The fd to stat
 *		uap->ub				The user stat buffer
 *		uap->xsecurity			The user extended security
 *						buffer, or 0 if none
 *		uap->xsecurity_size		The size of xsecurity, or 0
 *
 * Returns:	0				Success
 *		!0				Errno (see fstat)
 */
int
sys_fstat64_extended(proc_t p, struct fstat64_extended_args *uap, __unused int32_t *retval)
{
	return fstat(p, uap->fd, uap->ub, uap->xsecurity, uap->xsecurity_size, 1);
}


/*
 * sys_fstat64
 *
 * Description:	Get 64 bit version of the file status for the file associated
 *		with fd
 *
 * Parameters:	p				The process doing the fstat
 *		uap->fd				The fd to stat
 *		uap->ub				The user stat buffer
 *
 * Returns:	0				Success
 *		!0				Errno (see fstat)
 */
int
sys_fstat64(proc_t p, struct fstat64_args *uap, __unused int32_t *retval)
{
	return fstat(p, uap->fd, uap->ub, 0, 0, 1);
}


/*
 * sys_fpathconf
 *
 * Description:	Return pathconf information about a file descriptor.
 *
 * Parameters:	p				Process making the request
 *		uap->fd				fd to get information about
 *		uap->name			Name of information desired
 *		retval				Pointer to the call return area
 *
 * Returns:	0				Success
 *		EINVAL
 *	fp_lookup:EBADF				Bad file descriptor
 *	vnode_getwithref:???
 *	vn_pathconf:???
 *
 * Implicit returns:
 *		*retval (modified)		Returned information (numeric)
 */
int
sys_fpathconf(proc_t p, struct fpathconf_args *uap, int32_t *retval)
{
	int fd = uap->fd;
	struct fileproc *fp;
	struct vnode *vp;
	int error = 0;
	file_type_t type;


	AUDIT_ARG(fd, uap->fd);
	if ((error = fp_lookup(p, fd, &fp, 0))) {
		return error;
	}
	type = fp->f_type;

	switch (type) {
	case DTYPE_SOCKET:
		if (uap->name != _PC_PIPE_BUF) {
			error = EINVAL;
			goto out;
		}
		*retval = PIPE_BUF;
		error = 0;
		goto out;

	case DTYPE_PIPE:
		if (uap->name != _PC_PIPE_BUF) {
			error = EINVAL;
			goto out;
		}
		*retval = PIPE_BUF;
		error = 0;
		goto out;

	case DTYPE_VNODE:
		vp = (struct vnode *)fp_get_data(fp);

		if ((error = vnode_getwithref(vp)) == 0) {
			AUDIT_ARG(vnpath, vp, ARG_VNODE1);

			error = vn_pathconf(vp, uap->name, retval, vfs_context_current());

			(void)vnode_put(vp);
		}
		goto out;

	default:
		error = EINVAL;
		goto out;
	}
	/*NOTREACHED*/
out:
	fp_drop(p, fd, fp, 0);
	return error;
}

/*
 * sys_flock
 *
 * Description:	Apply an advisory lock on a file descriptor.
 *
 * Parameters:	p				Process making request
 *		uap->fd				fd on which the lock is to be
 *						attempted
 *		uap->how			(Un)Lock bits, including type
 *		retval				Pointer to the call return area
 *
 * Returns:	0				Success
 *	fp_getfvp:EBADF				Bad file descriptor
 *	fp_getfvp:ENOTSUP			fd does not refer to a vnode
 *	vnode_getwithref:???
 *	VNOP_ADVLOCK:???
 *
 * Implicit returns:
 *		*retval (modified)		Size of dtable
 *
 * Notes:	Just attempt to get a record lock of the requested type on
 *		the entire file (l_whence = SEEK_SET, l_start = 0, l_len = 0).
 */
int
sys_flock(proc_t p, struct flock_args *uap, __unused int32_t *retval)
{
	int fd = uap->fd;
	int how = uap->how;
	struct fileproc *fp;
	struct vnode *vp;
	struct flock lf;
	vfs_context_t ctx = vfs_context_current();
	int error = 0;

	AUDIT_ARG(fd, uap->fd);
	if ((error = fp_getfvp(p, fd, &fp, &vp))) {
		return error;
	}
	if ((error = vnode_getwithref(vp))) {
		goto out1;
	}
	AUDIT_ARG(vnpath, vp, ARG_VNODE1);

	lf.l_whence = SEEK_SET;
	lf.l_start = 0;
	lf.l_len = 0;
	if (how & LOCK_UN) {
		lf.l_type = F_UNLCK;
		error = VNOP_ADVLOCK(vp, (caddr_t)fp->fp_glob, F_UNLCK, &lf, F_FLOCK, ctx, NULL);
		goto out;
	}
	if (how & LOCK_EX) {
		lf.l_type = F_WRLCK;
	} else if (how & LOCK_SH) {
		lf.l_type = F_RDLCK;
	} else {
		error = EBADF;
		goto out;
	}
#if CONFIG_MACF
	error = mac_file_check_lock(kauth_cred_get(), fp->fp_glob, F_SETLK, &lf);
	if (error) {
		goto out;
	}
#endif
	error = VNOP_ADVLOCK(vp, (caddr_t)fp->fp_glob, F_SETLK, &lf,
	    (how & LOCK_NB ? F_FLOCK : F_FLOCK | F_WAIT),
	    ctx, NULL);
	if (!error) {
		os_atomic_or(&fp->fp_glob->fg_flag, FWASLOCKED, relaxed);
	}
out:
	(void)vnode_put(vp);
out1:
	fp_drop(p, fd, fp, 0);
	return error;
}

/*
 * sys_fileport_makeport
 *
 * Description: Obtain a Mach send right for a given file descriptor.
 *
 * Parameters:	p		Process calling fileport
 *              uap->fd		The fd to reference
 *              uap->portnamep  User address at which to place port name.
 *
 * Returns:	0		Success.
 *              EBADF		Bad file descriptor.
 *              EINVAL		File descriptor had type that cannot be sent, misc. other errors.
 *              EFAULT		Address at which to store port name is not valid.
 *              EAGAIN		Resource shortage.
 *
 * Implicit returns:
 *		On success, name of send right is stored at user-specified address.
 */
int
sys_fileport_makeport(proc_t p, struct fileport_makeport_args *uap,
    __unused int *retval)
{
	int err;
	int fd = uap->fd;
	user_addr_t user_portaddr = uap->portnamep;
	struct fileproc *fp = FILEPROC_NULL;
	struct fileglob *fg = NULL;
	ipc_port_t fileport;
	mach_port_name_t name = MACH_PORT_NULL;

	proc_fdlock(p);
	err = fp_lookup(p, fd, &fp, 1);
	if (err != 0) {
		goto out_unlock;
	}

	fg = fp->fp_glob;
	if (!fg_sendable(fg)) {
		err = EINVAL;
		goto out_unlock;
	}

	if (fp_isguarded(fp, GUARD_FILEPORT)) {
		err = fp_guard_exception(p, fd, fp, kGUARD_EXC_FILEPORT);
		goto out_unlock;
	}

	/* Dropped when port is deallocated */
	fg_ref(p, fg);

	proc_fdunlock(p);

	/* Allocate and initialize a port */
	fileport = fileport_alloc(fg);
	if (fileport == IPC_PORT_NULL) {
		fg_drop_live(fg);
		err = EAGAIN;
		goto out;
	}

	/* Add an entry.  Deallocates port on failure. */
	name = ipc_port_copyout_send(fileport, get_task_ipcspace(proc_task(p)));
	if (!MACH_PORT_VALID(name)) {
		err = EINVAL;
		goto out;
	}

	err = copyout(&name, user_portaddr, sizeof(mach_port_name_t));
	if (err != 0) {
		goto out;
	}

	/* Tag the fileglob for debugging purposes */
	lck_mtx_lock_spin(&fg->fg_lock);
	fg->fg_lflags |= FG_PORTMADE;
	lck_mtx_unlock(&fg->fg_lock);

	fp_drop(p, fd, fp, 0);

	return 0;

out_unlock:
	proc_fdunlock(p);
out:
	if (MACH_PORT_VALID(name)) {
		/* Don't care if another thread races us to deallocate the entry */
		(void) mach_port_deallocate(get_task_ipcspace(proc_task(p)), name);
	}

	if (fp != FILEPROC_NULL) {
		fp_drop(p, fd, fp, 0);
	}

	return err;
}

void
fileport_releasefg(struct fileglob *fg)
{
	(void)fg_drop(FG_NOPROC, fg);
}

/*
 * fileport_makefd
 *
 * Description: Obtain the file descriptor for a given Mach send right.
 *
 * Returns:	0		Success
 *		EINVAL		Invalid Mach port name, or port is not for a file.
 *	fdalloc:EMFILE
 *	fdalloc:ENOMEM		Unable to allocate fileproc or extend file table.
 *
 * Implicit returns:
 *		*retval (modified)		The new descriptor
 */
int
fileport_makefd(proc_t p, ipc_port_t port, fileproc_flags_t fp_flags, int *retval)
{
	struct fileglob *fg;
	struct fileproc *fp = FILEPROC_NULL;
	int fd;
	int err;

	fg = fileport_port_to_fileglob(port);
	if (fg == NULL) {
		err = EINVAL;
		goto out;
	}

	fp = fileproc_alloc_init();

	proc_fdlock(p);
	err = fdalloc(p, 0, &fd);
	if (err != 0) {
		proc_fdunlock(p);
		goto out;
	}
	if (fp_flags) {
		fp->fp_flags |= fp_flags;
	}

	fp->fp_glob = fg;
	fg_ref(p, fg);

	procfdtbl_releasefd(p, fd, fp);
	proc_fdunlock(p);

	*retval = fd;
	err = 0;
out:
	if ((fp != NULL) && (0 != err)) {
		fileproc_free(fp);
	}

	return err;
}

/*
 * sys_fileport_makefd
 *
 * Description: Obtain the file descriptor for a given Mach send right.
 *
 * Parameters:	p		Process calling fileport
 *              uap->port	Name of send right to file port.
 *
 * Returns:	0		Success
 *		EINVAL		Invalid Mach port name, or port is not for a file.
 *	fdalloc:EMFILE
 *	fdalloc:ENOMEM		Unable to allocate fileproc or extend file table.
 *
 * Implicit returns:
 *		*retval (modified)		The new descriptor
 */
int
sys_fileport_makefd(proc_t p, struct fileport_makefd_args *uap, int32_t *retval)
{
	ipc_port_t port = IPC_PORT_NULL;
	mach_port_name_t send = uap->port;
	kern_return_t res;
	int err;

	res = ipc_typed_port_copyin_send(get_task_ipcspace(proc_task(p)),
	    send, IKOT_FILEPORT, &port);

	if (res == KERN_SUCCESS) {
		err = fileport_makefd(p, port, FP_CLOEXEC, retval);
	} else {
		err = EINVAL;
	}

	if (IPC_PORT_NULL != port) {
		ipc_typed_port_release_send(port, IKOT_FILEPORT);
	}

	return err;
}


#pragma mark fileops wrappers

/*
 * fo_read
 *
 * Description:	Generic fileops read indirected through the fileops pointer
 *		in the fileproc structure
 *
 * Parameters:	fp				fileproc structure pointer
 *		uio				user I/O structure pointer
 *		flags				FOF_ flags
 *		ctx				VFS context for operation
 *
 * Returns:	0				Success
 *		!0				Errno from read
 */
int
fo_read(struct fileproc *fp, struct uio *uio, int flags, vfs_context_t ctx)
{
	return (*fp->f_ops->fo_read)(fp, uio, flags, ctx);
}

int
fo_no_read(struct fileproc *fp, struct uio *uio, int flags, vfs_context_t ctx)
{
#pragma unused(fp, uio, flags, ctx)
	return ENXIO;
}


/*
 * fo_write
 *
 * Description:	Generic fileops write indirected through the fileops pointer
 *		in the fileproc structure
 *
 * Parameters:	fp				fileproc structure pointer
 *		uio				user I/O structure pointer
 *		flags				FOF_ flags
 *		ctx				VFS context for operation
 *
 * Returns:	0				Success
 *		!0				Errno from write
 */
int
fo_write(struct fileproc *fp, struct uio *uio, int flags, vfs_context_t ctx)
{
	return (*fp->f_ops->fo_write)(fp, uio, flags, ctx);
}

int
fo_no_write(struct fileproc *fp, struct uio *uio, int flags, vfs_context_t ctx)
{
#pragma unused(fp, uio, flags, ctx)
	return ENXIO;
}


/*
 * fo_ioctl
 *
 * Description:	Generic fileops ioctl indirected through the fileops pointer
 *		in the fileproc structure
 *
 * Parameters:	fp				fileproc structure pointer
 *		com				ioctl command
 *		data				pointer to internalized copy
 *						of user space ioctl command
 *						parameter data in kernel space
 *		ctx				VFS context for operation
 *
 * Returns:	0				Success
 *		!0				Errno from ioctl
 *
 * Locks:	The caller is assumed to have held the proc_fdlock; this
 *		function releases and reacquires this lock.  If the caller
 *		accesses data protected by this lock prior to calling this
 *		function, it will need to revalidate/reacquire any cached
 *		protected data obtained prior to the call.
 */
int
fo_ioctl(struct fileproc *fp, u_long com, caddr_t data, vfs_context_t ctx)
{
	int error;

	proc_fdunlock(vfs_context_proc(ctx));
	error = (*fp->f_ops->fo_ioctl)(fp, com, data, ctx);
	proc_fdlock(vfs_context_proc(ctx));
	return error;
}

int
fo_no_ioctl(struct fileproc *fp, u_long com, caddr_t data, vfs_context_t ctx)
{
#pragma unused(fp, com, data, ctx)
	return ENOTTY;
}


/*
 * fo_select
 *
 * Description:	Generic fileops select indirected through the fileops pointer
 *		in the fileproc structure
 *
 * Parameters:	fp				fileproc structure pointer
 *		which				select which
 *		wql				pointer to wait queue list
 *		ctx				VFS context for operation
 *
 * Returns:	0				Success
 *		!0				Errno from select
 */
int
fo_select(struct fileproc *fp, int which, void *wql, vfs_context_t ctx)
{
	return (*fp->f_ops->fo_select)(fp, which, wql, ctx);
}

int
fo_no_select(struct fileproc *fp, int which, void *wql, vfs_context_t ctx)
{
#pragma unused(fp, which, wql, ctx)
	return ENOTSUP;
}


/*
 * fo_close
 *
 * Description:	Generic fileops close indirected through the fileops pointer
 *		in the fileproc structure
 *
 * Parameters:	fp				fileproc structure pointer for
 *						file to close
 *		ctx				VFS context for operation
 *
 * Returns:	0				Success
 *		!0				Errno from close
 */
int
fo_close(struct fileglob *fg, vfs_context_t ctx)
{
	return (*fg->fg_ops->fo_close)(fg, ctx);
}


/*
 * fo_drain
 *
 * Description:	Generic fileops kqueue filter indirected through the fileops
 *		pointer in the fileproc structure
 *
 * Parameters:	fp				fileproc structure pointer
 *		ctx				VFS context for operation
 *
 * Returns:	0				Success
 *		!0				errno from drain
 */
int
fo_drain(struct fileproc *fp, vfs_context_t ctx)
{
	return (*fp->f_ops->fo_drain)(fp, ctx);
}

int
fo_no_drain(struct fileproc *fp, vfs_context_t ctx)
{
#pragma unused(fp, ctx)
	return ENOTSUP;
}


/*
 * fo_kqfilter
 *
 * Description:	Generic fileops kqueue filter indirected through the fileops
 *		pointer in the fileproc structure
 *
 * Parameters:	fp				fileproc structure pointer
 *		kn				pointer to knote to filter on
 *
 * Returns:	(kn->kn_flags & EV_ERROR)	error in kn->kn_data
 *		0				Filter is not active
 *		!0				Filter is active
 */
int
fo_kqfilter(struct fileproc *fp, struct knote *kn, struct kevent_qos_s *kev)
{
	return (*fp->f_ops->fo_kqfilter)(fp, kn, kev);
}

int
fo_no_kqfilter(struct fileproc *fp, struct knote *kn, struct kevent_qos_s *kev)
{
#pragma unused(fp, kev)
	knote_set_error(kn, ENOTSUP);
	return 0;
}
