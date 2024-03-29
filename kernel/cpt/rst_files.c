/*
 *
 *  kernel/cpt/rst_files.c
 *
 *  Copyright (C) 2000-2005  SWsoft
 *  All rights reserved.
 *
 *  Licensing governed by "linux/COPYING.SWsoft" file.
 *
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/nsproxy.h>
#include <linux/major.h>
#include <linux/pipe_fs_i.h>
#include <linux/fs_struct.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/tty.h>
#include <linux/namei.h>
#include <linux/vmalloc.h>
#include <linux/smp_lock.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <asm/uaccess.h>
#include <bc/kmem.h>
#include <linux/cpt_image.h>
#include <linux/mnt_namespace.h>
#include <linux/fdtable.h>
#include <linux/shm.h>
#include <linux/signalfd.h>
#include <linux/proc_fs.h>
#include <linux/init_task.h>

#include "cpt_obj.h"
#include "cpt_context.h"
#include "cpt_mm.h"
#include "cpt_files.h"
#include "cpt_kernel.h"
#include "cpt_fsmagic.h"

#include "cpt_syscalls.h"


struct filejob {
	struct filejob *next;
	int	pid;
	loff_t	fdi;
};

static int rst_filejob_queue(loff_t pos, cpt_context_t *ctx)
{
	struct filejob *j;

	j = kmalloc(sizeof(*j), GFP_KERNEL);
	if (j == NULL)
		return -ENOMEM;
	j->pid = current->pid;
	j->fdi = pos;
	j->next = ctx->filejob_queue;
	ctx->filejob_queue = j;
	return 0;
}

static void _anon_pipe_buf_release(struct pipe_inode_info *pipe,
				  struct pipe_buffer *buf)
{
	struct page *page = buf->page;

	/*
	 * If nobody else uses this page, and we don't already have a
	 * temporary page, let's keep track of it as a one-deep
	 * allocation cache. (Otherwise just release our reference to it)
	 */
	if (page_count(page) == 1 && !pipe->tmp_page)
		pipe->tmp_page = page;
	else
		page_cache_release(page);

	module_put(THIS_MODULE);
}

static void *_anon_pipe_buf_map(struct pipe_inode_info *pipe,
			   struct pipe_buffer *buf, int atomic)
{
	if (atomic) {
		buf->flags |= PIPE_BUF_FLAG_ATOMIC;
		return kmap_atomic(buf->page, KM_USER0);
	}

	return kmap(buf->page);
}

static void _anon_pipe_buf_unmap(struct pipe_inode_info *pipe,
			    struct pipe_buffer *buf, void *map_data)
{
	if (buf->flags & PIPE_BUF_FLAG_ATOMIC) {
		buf->flags &= ~PIPE_BUF_FLAG_ATOMIC;
		kunmap_atomic(map_data, KM_USER0);
	} else
		kunmap(buf->page);
}

static int _anon_pipe_buf_steal(struct pipe_inode_info *pipe,
			   struct pipe_buffer *buf)
{
	struct page *page = buf->page;

	if (page_count(page) == 1) {
		lock_page(page);
		return 0;
	}

	return 1;
}

static void _anon_pipe_buf_get(struct pipe_inode_info *info, struct pipe_buffer *buf)
{
	page_cache_get(buf->page);
}

static int _anon_pipe_buf_confirm(struct pipe_inode_info *info, struct pipe_buffer *buf)
{
	return 0;
}

static struct pipe_buf_operations _anon_pipe_buf_ops = {
	.can_merge = 1,
	.map = _anon_pipe_buf_map,
	.unmap = _anon_pipe_buf_unmap,
	.release = _anon_pipe_buf_release,
	.confirm = _anon_pipe_buf_confirm,
	.get = _anon_pipe_buf_get,
	.steal = _anon_pipe_buf_steal,
};

/* Sorta ugly... Multiple readers/writers of named pipe rewrite buffer
 * many times. We need to mark it in CPT_OBJ_INODE table in some way.
 */
static int fixup_pipe_data(struct file *file, struct cpt_file_image *fi,
			   struct cpt_context *ctx)
{
	struct inode *ino = file->f_dentry->d_inode;
	struct cpt_inode_image ii;
	struct cpt_obj_bits b;
	struct pipe_inode_info *info;
	int err;
	int count;

	if (!S_ISFIFO(ino->i_mode)) {
		eprintk_ctx("fixup_pipe_data: not a pipe %Ld\n", (long long)fi->cpt_inode);
		return -EINVAL;
	}
	if (fi->cpt_inode == CPT_NULL)
		return 0;

	err = rst_get_object(CPT_OBJ_INODE, fi->cpt_inode, &ii, ctx);
	if (err)
		return err;

	if (ii.cpt_next <= ii.cpt_hdrlen)
		return 0;

	err = rst_get_object(CPT_OBJ_BITS, fi->cpt_inode + ii.cpt_hdrlen, &b, ctx);
	if (err)
		return err;

	if (b.cpt_size == 0)
		return 0;

	mutex_lock(&ino->i_mutex);
	info = ino->i_pipe;
	if (info->nrbufs) {
		mutex_unlock(&ino->i_mutex);
		eprintk("pipe buffer is restored already\n");
		return -EINVAL;
	}
	info->curbuf = 0;
	count = 0;
	while (count < b.cpt_size) {
		struct pipe_buffer *buf = info->bufs + info->nrbufs;
		void * addr;
		int chars;

		chars = b.cpt_size - count;
		if (chars > PAGE_SIZE)
			chars = PAGE_SIZE;
		if (!try_module_get(THIS_MODULE)) {
			err = -EBUSY;
			break;
		}

		buf->page = alloc_page(GFP_HIGHUSER);
		if (buf->page == NULL) {
			err = -ENOMEM;
			break;
		}
		buf->ops = &_anon_pipe_buf_ops;
		buf->offset = 0;
		buf->len = chars;
		info->nrbufs++;
		addr = kmap(buf->page);
		err = ctx->pread(addr, chars, ctx,
				 fi->cpt_inode + ii.cpt_hdrlen + b.cpt_hdrlen + count);
		if (err)
			break;
		count += chars;
	}
	mutex_unlock(&ino->i_mutex);

	return err;
}

static int make_flags(struct cpt_file_image *fi)
{
	int flags = O_NOFOLLOW;
	switch (fi->cpt_mode&(FMODE_READ|FMODE_WRITE)) {
	case FMODE_READ|FMODE_WRITE:
		flags |= O_RDWR; break;
	case FMODE_WRITE:
		flags |= O_WRONLY; break;
	case FMODE_READ:
		flags |= O_RDONLY; break;
	default: break;
	}
	flags |= fi->cpt_flags&~(O_ACCMODE|O_CREAT|O_TRUNC|O_EXCL|FASYNC);
	flags |= O_NONBLOCK|O_NOCTTY;
	return flags;
}

static struct file *open_pipe(char *name,
			      struct cpt_file_image *fi,
			      unsigned flags,
			      struct cpt_context *ctx)
{
	int err;
	cpt_object_t *obj;
	struct cpt_inode_image ii;
	struct file *rf, *wf;

	err = rst_get_object(CPT_OBJ_INODE, fi->cpt_inode, &ii, ctx);
	if (err)
		return ERR_PTR(err);

	if (ii.cpt_sb == FSMAGIC_PIPEFS) {
		int pfd[2];

		if ((err = sc_pipe(pfd)) < 0)
			return ERR_PTR(err);

		rf = fcheck(pfd[0]);
		wf = fcheck(pfd[1]);
		get_file(rf);
		get_file(wf);
		sc_close(pfd[0]);
		sc_close(pfd[1]);

		if (fi->cpt_mode&FMODE_READ) {
			struct file *tf;
			tf = wf; wf = rf; rf = tf;
		}
	} else {
		if (fi->cpt_mode&FMODE_READ) {
			rf = filp_open(name, flags, 0);
			if (IS_ERR(rf)) {
				dprintk_ctx("filp_open\n");
				return rf;
			}
			dprintk_ctx(CPT_FID "open RDONLY fifo ino %Ld %p %x\n", CPT_TID(current),
				    (long long)fi->cpt_inode, rf, rf->f_dentry->d_inode->i_mode);
			return rf;
		}

		dprintk_ctx(CPT_FID "open WRONLY fifo ino %Ld\n", CPT_TID(current), (long long)fi->cpt_inode);

		rf = filp_open(name, O_RDWR|O_NONBLOCK, 0);
		if (IS_ERR(rf))
			return rf;
		wf = dentry_open(dget(rf->f_dentry),
				 mntget(rf->f_vfsmnt), flags, NULL);
	}

	/* Add pipe inode to obj table. */
	obj = cpt_object_add(CPT_OBJ_INODE, wf->f_dentry->d_inode, ctx);
	if (obj == NULL) {
		fput(rf); fput(wf);
		return ERR_PTR(-ENOMEM);
	}
	cpt_obj_setpos(obj, fi->cpt_inode, ctx);
	obj->o_parent = rf;

	/* Add another side of pipe to obj table, it will not be used
	 * (o_pos = PT_NULL), another processes opeining pipe will find
	 * inode and open it with dentry_open(). */
	obj = cpt_object_add(CPT_OBJ_FILE, rf, ctx);
	if (obj == NULL) {
		fput(wf);
		return ERR_PTR(-ENOMEM);
	}
	return wf;
}

static struct file *open_special(struct cpt_file_image *fi,
				 unsigned flags,
				 int deleted,
				 struct cpt_context *ctx)
{
	struct cpt_inode_image *ii;
	struct file *file;

	/* Directories and named pipes are not special actually */
	if (S_ISDIR(fi->cpt_i_mode) || S_ISFIFO(fi->cpt_i_mode))
		return NULL;

	/* No support for block devices at the moment. */
	if (S_ISBLK(fi->cpt_i_mode))
		return ERR_PTR(-EINVAL);

	if (S_ISSOCK(fi->cpt_i_mode)) {
		eprintk_ctx("bug: socket is not open\n");
		return ERR_PTR(-EINVAL);
	}

	/* Support only (some) character devices at the moment. */
	if (!S_ISCHR(fi->cpt_i_mode))
		return ERR_PTR(-EINVAL);

	ii = __rst_get_object(CPT_OBJ_INODE, fi->cpt_inode, ctx);
	if (ii == NULL)
		return ERR_PTR(-ENOMEM);

	/* Do not worry about this right now. /dev/null,zero,*random are here.
	 * To prohibit at least /dev/mem?
	 */
	if (MAJOR(ii->cpt_rdev) == MEM_MAJOR) {
		kfree(ii);
		return NULL;
	}

	/* /dev/net/tun will be opened by caller */
	if (fi->cpt_lflags & CPT_DENTRY_TUNTAP) {
		kfree(ii);
		return NULL;
	}	

	file = rst_open_tty(fi, ii, flags, ctx);
	kfree(ii);
	return file;
}

static int restore_posix_lock(struct file *file, struct cpt_flock_image *fli, cpt_context_t *ctx)
{
	struct file_lock lock;
	cpt_object_t *obj;

	memset(&lock, 0, sizeof(lock));
	lock.fl_type = fli->cpt_type;
	lock.fl_flags = fli->cpt_flags & ~FL_SLEEP;
	lock.fl_start = fli->cpt_start;
	lock.fl_end = fli->cpt_end;
	obj = lookup_cpt_obj_byindex(CPT_OBJ_FILES, fli->cpt_owner, ctx);
	if (!obj) {
		eprintk_ctx("unknown lock owner %d\n", (int)fli->cpt_owner);
		return -EINVAL;
	}
	lock.fl_owner = obj->o_obj;
	lock.fl_pid = vpid_to_pid(fli->cpt_pid);
	if (lock.fl_pid < 0) {
		eprintk_ctx("unknown lock pid %d\n", lock.fl_pid);
		return -EINVAL;
	}
	lock.fl_file = file;

	if (lock.fl_owner == NULL)
		eprintk_ctx("no lock owner\n");
	return posix_lock_file(file, &lock, NULL);
}

static int restore_flock(struct file *file, struct cpt_flock_image *fli,
			 cpt_context_t *ctx)
{
	int cmd, err, fd;
	fd = get_unused_fd();
	if (fd < 0) {
		eprintk_ctx("BSD flock cannot be restored\n");
		return fd;
	}
	get_file(file);
	fd_install(fd, file);
	if (fli->cpt_type == F_RDLCK) {
		cmd = LOCK_SH;
	} else if (fli->cpt_type == F_WRLCK) {
		cmd = LOCK_EX;
	} else {
		eprintk_ctx("flock flavor is unknown: %u\n", fli->cpt_type);
		sc_close(fd);
		return -EINVAL;
	}

	err = sc_flock(fd, LOCK_NB | cmd);
	sc_close(fd);
	return err;
}


static int fixup_posix_locks(struct file *file,
			     struct cpt_file_image *fi,
			     loff_t pos, struct cpt_context *ctx)
{
	int err;
	loff_t end;
	struct cpt_flock_image fli;

	end = pos + fi->cpt_next;
	pos += fi->cpt_hdrlen;
	while (pos < end) {
		err = rst_get_object(-1, pos, &fli, ctx);
		if (err)
			return err;
		if (fli.cpt_object == CPT_OBJ_FLOCK &&
		    (fli.cpt_flags&FL_POSIX)) {
			err = restore_posix_lock(file, &fli, ctx);
			if (err)
				return err;
			dprintk_ctx("posix lock restored\n");
		}
		pos += fli.cpt_next;
	}
	return 0;
}

int rst_posix_locks(struct cpt_context *ctx)
{
	int err;
	cpt_object_t *obj;

	for_each_object(obj, CPT_OBJ_FILE) {
		struct file *file = obj->o_obj;
		struct cpt_file_image fi;

		if (obj->o_pos == CPT_NULL)
			continue;

		err = rst_get_object(CPT_OBJ_FILE, obj->o_pos, &fi, ctx);
		if (err < 0)
			return err;
		if (fi.cpt_next > fi.cpt_hdrlen)
			fixup_posix_locks(file, &fi, obj->o_pos, ctx);
	}
	return 0;
}

static int fixup_flocks(struct file *file,
			struct cpt_file_image *fi,
			loff_t pos, struct cpt_context *ctx)
{
	int err;
	loff_t end;
	struct cpt_flock_image fli;

	end = pos + fi->cpt_next;
	pos += fi->cpt_hdrlen;
	while (pos < end) {
		err = rst_get_object(-1, pos, &fli, ctx);
		if (err)
			return err;
		if (fli.cpt_object == CPT_OBJ_FLOCK &&
		    (fli.cpt_flags&FL_FLOCK)) {
			err = restore_flock(file, &fli, ctx);
			if (err)
				return err;
			dprintk_ctx("bsd lock restored\n");
		}
		pos += fli.cpt_next;
	}
	return 0;
}


static int fixup_reg_data(struct file *file, loff_t pos, loff_t end,
			  struct cpt_context *ctx)
{
	int err;
	struct cpt_page_block pgb;
	ssize_t (*do_write)(struct file *, const char __user *, size_t, loff_t *ppos);

	do_write = file->f_op->write;
	if (do_write == NULL) {
		eprintk_ctx("no write method. Cannot restore contents of the file.\n");
		return -EINVAL;
	}

	atomic_long_inc(&file->f_count);

	while (pos < end) {
		loff_t opos;
		loff_t ipos;
		int count;

		err = rst_get_object(CPT_OBJ_PAGES, pos, &pgb, ctx);
		if (err)
			goto out;
		dprintk_ctx("restoring file data block: %08x-%08x\n",
		       (__u32)pgb.cpt_start, (__u32)pgb.cpt_end);
		ipos = pos + pgb.cpt_hdrlen;
		opos = pgb.cpt_start;
		count = pgb.cpt_end-pgb.cpt_start;
		while (count > 0) {
			mm_segment_t oldfs;
			int copy = count;

			if (copy > PAGE_SIZE)
				copy = PAGE_SIZE;
			(void)cpt_get_buf(ctx);
			oldfs = get_fs(); set_fs(KERNEL_DS);
			err = ctx->pread(ctx->tmpbuf, copy, ctx, ipos);
			set_fs(oldfs);
			if (err) {
				__cpt_release_buf(ctx);
				goto out;
			}
			if (!(file->f_mode & FMODE_WRITE) ||
			    (file->f_flags&O_DIRECT)) {
				fput(file);
				file = dentry_open(dget(file->f_dentry),
						   mntget(file->f_vfsmnt),
						   O_WRONLY | O_LARGEFILE, NULL);
				if (IS_ERR(file)) {
					__cpt_release_buf(ctx);
					return PTR_ERR(file);
				}
			}
			oldfs = get_fs(); set_fs(KERNEL_DS);
			ipos += copy;
			err = do_write(file, ctx->tmpbuf, copy, &opos);
			set_fs(oldfs);
			__cpt_release_buf(ctx);
			if (err != copy) {
				if (err >= 0)
					err = -EIO;
				goto out;
			}
			count -= copy;
		}
		pos += pgb.cpt_next;
	}
	err = 0;

out:
	fput(file);
	return err;
}


static int fixup_file_content(struct file **file_p, struct cpt_file_image *fi,
			      struct cpt_inode_image *ii,
			      struct cpt_context *ctx)
{
	int err;
	struct file *file = *file_p;
	struct iattr newattrs;

	if (!S_ISREG(fi->cpt_i_mode))
		return 0;

	if (file == NULL) {
		file = shmem_file_setup("dev/zero", ii->cpt_size, 0);
		if (IS_ERR(file))
			return PTR_ERR(file);
		*file_p = file;
	}

	if (ii->cpt_next > ii->cpt_hdrlen) {
		struct cpt_object_hdr hdr;
		err = ctx->pread(&hdr, sizeof(struct cpt_object_hdr), ctx, fi->cpt_inode+ii->cpt_hdrlen);
		if (err)
			return err;
		if (hdr.cpt_object == CPT_OBJ_PAGES) {
			err = fixup_reg_data(file, fi->cpt_inode+ii->cpt_hdrlen,
					fi->cpt_inode+ii->cpt_next, ctx);
			if (err)
				return err;
		}
	}

	mutex_lock(&file->f_dentry->d_inode->i_mutex);
	/* stage 1 - update size like do_truncate does */
	newattrs.ia_valid = ATTR_SIZE | ATTR_CTIME;
	newattrs.ia_size = ii->cpt_size;
	cpt_timespec_import(&newattrs.ia_ctime, ii->cpt_ctime);
	err = notify_change(file->f_dentry, &newattrs);
	if (err)
		goto out;

	/* stage 2 - update times, owner and mode */
	newattrs.ia_valid = ATTR_MTIME | ATTR_ATIME |
		ATTR_ATIME_SET | ATTR_MTIME_SET |
		ATTR_MODE | ATTR_UID | ATTR_GID;
	newattrs.ia_uid = ii->cpt_uid;
	newattrs.ia_gid = ii->cpt_gid;
	newattrs.ia_mode = file->f_dentry->d_inode->i_mode & S_IFMT;
	newattrs.ia_mode |= (ii->cpt_mode & ~S_IFMT);
	cpt_timespec_import(&newattrs.ia_atime, ii->cpt_atime);
	cpt_timespec_import(&newattrs.ia_mtime, ii->cpt_mtime);
	err = notify_change(file->f_dentry, &newattrs);

out:
	mutex_unlock(&file->f_dentry->d_inode->i_mutex);
	return err;
}

static int fixup_file_flags(struct file *file, const struct cred *cred,
			    struct cpt_file_image *fi,
			    int was_dentry_open, loff_t pos,
			    cpt_context_t *ctx)
{
	if (fi->cpt_pos != file->f_pos) {
		int err = -ESPIPE;
		if (file->f_op->llseek)
			err = file->f_op->llseek(file, fi->cpt_pos, 0);
		if (err < 0) {
			dprintk_ctx("file %Ld lseek %Ld - %Ld\n",
				    (long long)pos,
				    (long long)file->f_pos,
				    (long long)fi->cpt_pos);
			file->f_pos = fi->cpt_pos;
		}
	}

	if (cred->uid != fi->cpt_uid || cred->gid != fi->cpt_gid)
		wprintk_ctx("fixup_file_flags: oops... creds mismatch\n");

	/*
	 * this is wrong. but with current cpt_file_image there's
	 * nothing we can do
	 */

	put_cred(file->f_cred);
	file->f_cred = get_cred(cred);

	file->f_owner.pid = 0;
	if (fi->cpt_fown_pid != CPT_FOWN_STRAY_PID) {
		file->f_owner.pid = find_get_pid(fi->cpt_fown_pid);
		if (file->f_owner.pid == NULL) {
			wprintk_ctx("fixup_file_flags: owner %d does not exist anymore\n",
					fi->cpt_fown_pid);
			return -EINVAL;
		}
	}
	file->f_owner.uid = fi->cpt_fown_uid;
	file->f_owner.euid = fi->cpt_fown_euid;
	file->f_owner.signum = fi->cpt_fown_signo;

	if (file->f_mode != fi->cpt_mode) {
		if (was_dentry_open &&
		    ((file->f_mode^fi->cpt_mode)&(FMODE_PREAD|FMODE_LSEEK))) {
			file->f_mode &= ~(FMODE_PREAD|FMODE_LSEEK);
			file->f_mode |= fi->cpt_mode&(FMODE_PREAD|FMODE_LSEEK);
		}
		if (file->f_mode != fi->cpt_mode)
			wprintk_ctx("file %ld mode mismatch %08x %08x\n", (long)pos, file->f_mode, fi->cpt_mode);
	}
	if (file->f_flags != fi->cpt_flags) {
		if (!(fi->cpt_flags&O_NOFOLLOW))
			file->f_flags &= ~O_NOFOLLOW;
		if ((file->f_flags^fi->cpt_flags)&O_NONBLOCK) {
			file->f_flags &= ~O_NONBLOCK;
			file->f_flags |= fi->cpt_flags&O_NONBLOCK;
		}
		if (fi->cpt_flags&FASYNC) {
			if (fi->cpt_fown_fd == -1) {
				wprintk_ctx("No fd for FASYNC\n");
				return -EINVAL;
			} else if (file->f_op && file->f_op->fasync) {
				if (file->f_op->fasync(fi->cpt_fown_fd, file, 1) < 0) {
					wprintk_ctx("FASYNC problem\n");
					return -EINVAL;
				} else {
					file->f_flags |= FASYNC;
				}
			}
		}
		if (file->f_flags != fi->cpt_flags) {
			eprintk_ctx("file %ld flags mismatch %08x %08x\n", (long)pos, file->f_flags, fi->cpt_flags);
			return -EINVAL;
		}
	}
	return 0;
}

static struct file *
open_deleted(char *name, unsigned flags, struct cpt_file_image *fi,
	     struct cpt_inode_image *ii, cpt_context_t *ctx)
{
	struct file * file;
	char *suffix = NULL;
	int attempt = 0;
	int tmp_pass = 0;
	mode_t mode = fi->cpt_i_mode;

	/* Strip (deleted) part... */
	if (strlen(name) > strlen(" (deleted)")) {
		if (strcmp(name + strlen(name) - strlen(" (deleted)"), " (deleted)") == 0) {
			suffix = &name[strlen(name) - strlen(" (deleted)")];
			*suffix = 0;
		} else if (memcmp(name, "(deleted) ", strlen("(deleted) ")) == 0) {
			memmove(name, name + strlen("(deleted) "), strlen(name) - strlen(" (deleted)") + 1);
			suffix = name + strlen(name);
		}
	}

try_again:
	for (;;) {
		if (attempt) {
			if (attempt > 1000) {
				eprintk_ctx("open_deleted: failed after %d attempts\n", attempt);
				return ERR_PTR(-EEXIST);
			}
			if (suffix == NULL) {
				eprintk_ctx("open_deleted: no suffix\n");
				return ERR_PTR(-EEXIST);
			}
			sprintf(suffix, ".%08x", (unsigned)((xtime.tv_nsec>>10)+attempt));
		}
		attempt++;

		if (S_ISFIFO(mode)) {
			int err;
			err = sc_mknod(name, S_IFIFO|(mode&017777), 0);
			if (err == -EEXIST)
				continue;
			if (err < 0 && !tmp_pass)
				goto change_dir;
			if (err < 0)
				return ERR_PTR(err);
			file = open_pipe(name, fi, flags, ctx);
			sc_unlink(name);
		} else if (S_ISCHR(mode)) {
			int err;
			err = sc_mknod(name, S_IFCHR|(mode&017777), new_encode_dev(ii->cpt_rdev));
			if (err == -EEXIST)
				continue;
			if (err < 0 && !tmp_pass)
				goto change_dir;
			if (err < 0)
				return ERR_PTR(err);
			file = filp_open(name, flags, mode&017777);
			sc_unlink(name);
		} else if (S_ISDIR(mode)) {
			int err;
			err = sc_mkdir(name, mode&017777);
			if (err == -EEXIST)
				continue;
			if (err < 0 && !tmp_pass)
				goto change_dir;
			if (err < 0)
				return ERR_PTR(err);
			file = filp_open(name, flags, mode&017777);
			sc_rmdir(name);
		} else {
			file = filp_open(name, O_CREAT|O_EXCL|flags, mode&017777);
			if (IS_ERR(file)) {
				if (PTR_ERR(file) == -EEXIST)
					continue;
				if (!tmp_pass)
					goto change_dir;
			} else {
				sc_unlink(name);
			}
		}
		break;
	}

	if (IS_ERR(file)) {
		eprintk_ctx("filp_open %s: %ld\n", name, PTR_ERR(file));
		return file;
	} else {
		dprintk_ctx("deleted file created as %s, %p, %x\n", name, file, file->f_dentry->d_inode->i_mode);
	}
	return file;

change_dir:
	sprintf(name, "/tmp/rst%u", current->pid);
	suffix = name + strlen(name);
	attempt = 1;
	tmp_pass = 1;
	goto try_again;
}

#ifdef CONFIG_SIGNALFD
static struct file *open_signalfd(struct cpt_file_image *fi, int flags, struct cpt_context *ctx)
{
	sigset_t mask;
	mm_segment_t old_fs;
	int fd;
	struct file *file;

	cpt_sigset_import(&mask, fi->cpt_priv);

	old_fs = get_fs(); set_fs(KERNEL_DS);
	fd = do_signalfd(-1, &mask, flags & (O_CLOEXEC | O_NONBLOCK));
	set_fs(old_fs);

	if (fd < 0)
		return ERR_PTR(fd);

	file = fget(fd);
	sys_close(fd);

	return file;
}
#else
static struct file *open_signalfd(struct cpt_file_image *fi, int flags, struct cpt_context *ctx)
{
	return ERR_PTR(-EINVAL);
}
#endif

struct file *rst_file(loff_t pos, int fd, struct cpt_context *ctx)
{
	int err;
	int was_dentry_open = 0;
	cpt_object_t *obj;
	cpt_object_t *iobj;
	struct cpt_file_image fi;
	__u8 *name = NULL;
	struct file *file;
	struct proc_dir_entry *proc_dead_file;
	int flags;
	const struct cred *cred_origin;

	/*
	 * It may happen that a process which created a file
	 * had changed its UID after that (keeping file opened/referenced
	 * with write permissions for 'own' only) as a result we might
	 * be unable to read it at restore time due to credentials
	 * mismatch, to break this tie we temporary take init_cred credentials
	 * and as only the file gets read into the memory we restore original
	 * credentials back
	 *
	 * Same time if between credentials rise/restore you need
	 * the former credentials (for fixups or whatever) --
	 * use cred_origin for that
	 */

	cred_origin = override_creds(&init_cred);

	obj = lookup_cpt_obj_bypos(CPT_OBJ_FILE, pos, ctx);
	if (obj) {
		file = obj->o_obj;
		if (obj->o_index >= 0) {
			dprintk_ctx("file is attached to a socket\n");
			err = rst_get_object(CPT_OBJ_FILE, pos, &fi, ctx);
			if (err < 0)
				goto err_out;
			fixup_file_flags(file, cred_origin, &fi, 0, pos, ctx);
		}
		get_file(file);
		revert_creds(cred_origin);
		return file;
	}

	err = rst_get_object(CPT_OBJ_FILE, pos, &fi, ctx);
	if (err < 0)
		goto err_out;

	flags = make_flags(&fi);

	/* Easy way, inode has been already open. */
	if (fi.cpt_inode != CPT_NULL &&
	    !(fi.cpt_lflags & CPT_DENTRY_CLONING) &&
	    (iobj = lookup_cpt_obj_bypos(CPT_OBJ_INODE, fi.cpt_inode, ctx)) != NULL &&
	    iobj->o_parent) {
		struct file *filp = iobj->o_parent;
		file = dentry_open(dget(filp->f_dentry),
				   mntget(filp->f_vfsmnt), flags, NULL);
		dprintk_ctx("rst_file: file obtained by dentry_open\n");
		was_dentry_open = 1;
		goto map_file;
	}

	if (fi.cpt_next > fi.cpt_hdrlen)
		name = rst_get_name(pos + sizeof(fi), ctx);

	if (!name) {
		eprintk_ctx("no name for file?\n");
		err = -EINVAL;
		goto err_out;
	}

	if (fi.cpt_lflags & CPT_DENTRY_DELETED) {
		struct cpt_inode_image ii;
		if (fi.cpt_inode == CPT_NULL) {
			eprintk_ctx("deleted file and no inode.\n");
			err = -EINVAL;
			goto err_out;
		}

		err = rst_get_object(CPT_OBJ_INODE, fi.cpt_inode, &ii, ctx);
		if (err)
			goto err_out;

		if (ii.cpt_next > ii.cpt_hdrlen) {
			struct cpt_object_hdr hdr;
			err = ctx->pread(&hdr, sizeof(hdr), ctx,
					fi.cpt_inode + ii.cpt_hdrlen);
			if (err)
				goto err_out;
			if (hdr.cpt_object == CPT_OBJ_NAME) {
				rst_put_name(name, ctx);
				name = rst_get_name(fi.cpt_inode+ii.cpt_hdrlen,
						ctx);
				if (!name) {
					eprintk_ctx("no name for link?\n");
					err = -EINVAL;
					goto err_out;
				}
				if ((fi.cpt_lflags & CPT_DENTRY_HARDLINKED) &&
				    !ctx->hardlinked_on) {
					eprintk_ctx("Open hardlinked is off\n");
					err = -EPERM;
					goto err_out;
				}
				goto open_file;
			}
		}

		/* One very special case... */
		if (S_ISREG(fi.cpt_i_mode) &&
		    (!name[0] || strcmp(name, "/dev/zero (deleted)") == 0)) {
			/* MAP_ANON|MAP_SHARED mapping.
			 * kernel makes this damn ugly way, when file which
			 * is passed to mmap by user does not match
			 * file finally attached to VMA. Ok, rst_mm
			 * has to take care of this. Otherwise, it will fail.
			 */
			file = NULL;
		} else if (S_ISREG(fi.cpt_i_mode) ||
			   S_ISCHR(fi.cpt_i_mode) ||
			   S_ISFIFO(fi.cpt_i_mode) ||
			   S_ISDIR(fi.cpt_i_mode)) {
			if (S_ISCHR(fi.cpt_i_mode)) {
				file = open_special(&fi, flags, 1, ctx);
				if (file != NULL)
					goto map_file;
			}
			file = open_deleted(name, flags, &fi, &ii, ctx);
			if (IS_ERR(file))
				goto out;
		} else {
			eprintk_ctx("not a regular deleted file.\n");
			err = -EINVAL;
			goto err_out;
		}

		err = fixup_file_content(&file, &fi, &ii, ctx);
		if (err)
			goto err_put;
		goto map_file;
	} else {
open_file:
		if (!name[0]) {
			eprintk_ctx("empty name for file?\n");
			err = -EINVAL;
			goto err_out;
		}
		if ((fi.cpt_lflags & CPT_DENTRY_EPOLL) &&
		    (file = cpt_open_epolldev(&fi, flags, ctx)) != NULL)
			goto map_file;
#ifdef CONFIG_INOTIFY_USER
		if ((fi.cpt_lflags & CPT_DENTRY_INOTIFY) &&
		    (file = rst_open_inotify(&fi, flags, ctx)) != NULL)
			goto map_file;
#else
		if (fi.cpt_lflags & CPT_DENTRY_INOTIFY) {
			err = -EINVAL;
			goto err_out;
		}
#endif
		if ((fi.cpt_lflags & CPT_DENTRY_SIGNALFD) &&
			(file = open_signalfd(&fi, flags, ctx)) != NULL)
			goto map_file;
		if (S_ISFIFO(fi.cpt_i_mode) &&
		    (file = open_pipe(name, &fi, flags, ctx)) != NULL)
			goto map_file;
		if (!S_ISREG(fi.cpt_i_mode) &&
		    (file = open_special(&fi, flags, 0, ctx)) != NULL)
			goto map_file;
	}

	/* This hook is needed to open file /proc/<pid>/<somefile>
	 * but there is no proccess with pid <pid>.
	 */
	proc_dead_file = NULL;
	if (fi.cpt_lflags & CPT_DENTRY_PROCPID_DEAD) {
		sprintf(name, "/proc/rst_dead_pid_file_%d", task_pid_vnr(current));

		proc_dead_file = create_proc_entry(name + 6, S_IRUGO|S_IWUGO,
						   NULL);
		if (!proc_dead_file) {
			eprintk_ctx("can't create proc entry %s\n", name);
			err = -ENOMEM;
			goto err_out;
		}
#ifdef CONFIG_PROC_FS
		proc_dead_file->proc_fops = &dummy_proc_pid_file_operations;
#endif
	}

	file = filp_open(name, flags, 0);

	if (proc_dead_file) {
		remove_proc_entry(proc_dead_file->name, NULL);
		if (!IS_ERR(file))
			d_drop(file->f_dentry);
	}
map_file:
	if (!IS_ERR(file)) {
		fixup_file_flags(file, cred_origin, &fi, was_dentry_open, pos, ctx);

		if (S_ISFIFO(fi.cpt_i_mode) && !was_dentry_open) {
			err = fixup_pipe_data(file, &fi, ctx);
			if (err)
				goto err_put;
		}

		/* This is very special hack. Logically, cwd/root are
		 * nothing but open directories. Nevertheless, this causes
		 * failures of restores, when number of open files in VE
		 * is close to limit. So, if it is rst_file() of cwd/root
		 * (fd = -2) and the directory is not deleted, we skip
		 * adding files to object table. If the directory is
		 * not unlinked, this cannot cause any problems.
		 */
		if (fd != -2 ||
		    !S_ISDIR(file->f_dentry->d_inode->i_mode) ||
		    (fi.cpt_lflags & CPT_DENTRY_DELETED)) {
			obj = cpt_object_get(CPT_OBJ_FILE, file, ctx);
			if (!obj) {
				obj = cpt_object_add(CPT_OBJ_FILE, file, ctx);
				if (obj)
					get_file(file);
			}
			if (obj)
				cpt_obj_setpos(obj, pos, ctx);

			obj = cpt_object_add(CPT_OBJ_INODE, file->f_dentry->d_inode, ctx);
			if (obj) {
				cpt_obj_setpos(obj, fi.cpt_inode, ctx);
				if (!obj->o_parent || !(fi.cpt_lflags & CPT_DENTRY_DELETED))
					obj->o_parent = file;
			}
		}

		if (fi.cpt_next > fi.cpt_hdrlen) {
			err = fixup_flocks(file, &fi, pos, ctx);
			if (err)
				goto err_put;
		}
	} else {
		if ((fi.cpt_lflags & CPT_DENTRY_PROC) &&
		    !(fi.cpt_lflags & CPT_DENTRY_PROCPID_DEAD)) {
			dprintk_ctx("rst_file /proc delayed\n");
			file = NULL;
		} else if (name)
			eprintk_ctx("can't open file %s\n", name);
	}

out:
	if (name)
		rst_put_name(name, ctx);
	revert_creds(cred_origin);
	return file;

err_put:
	if (file)
		fput(file);
err_out:
	if (name)
		rst_put_name(name, ctx);
	revert_creds(cred_origin);
	return ERR_PTR(err);
}


__u32 rst_files_flag(struct cpt_task_image *ti, struct cpt_context *ctx)
{
	__u32 flag = 0;

	if (ti->cpt_files == CPT_NULL ||
	    lookup_cpt_obj_bypos(CPT_OBJ_FILES, ti->cpt_files, ctx))
		flag |= CLONE_FILES;
	if (ti->cpt_fs == CPT_NULL ||
	    lookup_cpt_obj_bypos(CPT_OBJ_FS, ti->cpt_fs, ctx))
		flag |= CLONE_FS;
	return flag;
}

static void local_close_files(struct files_struct * files)
{
	int i, j;

	j = 0;
	for (;;) {
		unsigned long set;
		i = j * __NFDBITS;
		if (i >= files->fdt->max_fds)
			break;
		set = files->fdt->open_fds->fds_bits[j];
		while (set) {
			if (set & 1) {
				struct file * file = xchg(&files->fdt->fd[i], NULL);
				if (file)
					filp_close(file, files);
			}
			i++;
			set >>= 1;
		}
		files->fdt->open_fds->fds_bits[j] = 0;
		files->fdt->close_on_exec->fds_bits[j] = 0;
		j++;
	}
}

extern int expand_fdtable(struct files_struct *files, int nr);


static int rst_files(struct cpt_task_image *ti, struct cpt_context *ctx,
		int from, int to)
{
	struct cpt_files_struct_image fi;
	struct files_struct *f = current->files;
	cpt_object_t *obj;
	loff_t pos, endpos;
	int err;

	if (ti->cpt_files == CPT_NULL) {
		current->files = NULL;
		if (f)
			put_files_struct(f);
		return 0;
	}

	if (from == 3) {
		err = rst_get_object(CPT_OBJ_FILES, ti->cpt_files, &fi, ctx);
		if (err)
			return err;

		goto just_do_it;
	}

	obj = lookup_cpt_obj_bypos(CPT_OBJ_FILES, ti->cpt_files, ctx);
	if (obj) {
		if (obj->o_obj != f) {
			put_files_struct(f);
			f = obj->o_obj;
			atomic_inc(&f->count);
			current->files = f;
		}
		return 0;
	}

	err = rst_get_object(CPT_OBJ_FILES, ti->cpt_files, &fi, ctx);
	if (err)
		return err;

	local_close_files(f);

	if (fi.cpt_max_fds > f->fdt->max_fds) {
		spin_lock(&f->file_lock);
		err = expand_fdtable(f, fi.cpt_max_fds-1);
		spin_unlock(&f->file_lock);
		if (err < 0)
			return err;
	}

just_do_it:
	pos = ti->cpt_files + fi.cpt_hdrlen;
	endpos = ti->cpt_files + fi.cpt_next;
	while (pos < endpos) {
		struct cpt_fd_image fdi;
		struct file *filp;

		err = rst_get_object(CPT_OBJ_FILEDESC, pos, &fdi, ctx);
		if (err)
			return err;
		if (fdi.cpt_fd < from || fdi.cpt_fd > to)
			goto skip;

		filp = rst_file(fdi.cpt_file, fdi.cpt_fd, ctx);
		if (IS_ERR(filp)) {
			eprintk_ctx("rst_file: %ld %Lu\n", PTR_ERR(filp),
				    (long long)fdi.cpt_file);
			return PTR_ERR(filp);
		}
		if (filp == NULL) {
			int err = rst_filejob_queue(pos, ctx);
			if (err)
				return err;
		} else {
			if (fdi.cpt_fd >= f->fdt->max_fds) BUG();
			f->fdt->fd[fdi.cpt_fd] = filp;
			FD_SET(fdi.cpt_fd, f->fdt->open_fds);
			if (fdi.cpt_flags&CPT_FD_FLAG_CLOSEEXEC)
				FD_SET(fdi.cpt_fd, f->fdt->close_on_exec);
		}

skip:
		pos += fdi.cpt_next;
	}
	f->next_fd = fi.cpt_next_fd;

	obj = cpt_object_add(CPT_OBJ_FILES, f, ctx);
	if (obj) {
		cpt_obj_setpos(obj, ti->cpt_files, ctx);
		cpt_obj_setindex(obj, fi.cpt_index, ctx);
	}
	return 0;
}

int rst_files_complete(struct cpt_task_image *ti, struct cpt_context *ctx)
{
	return rst_files(ti, ctx, (ti->cpt_pid == 1) ? 3 : 0, INT_MAX);
}

int rst_files_std(struct cpt_task_image *ti, struct cpt_context *ctx)
{
	return rst_files(ti, ctx, 0, 2);
}

int rst_do_filejobs(cpt_context_t *ctx)
{
	struct filejob *j;

	while ((j = ctx->filejob_queue) != NULL) {
		int err;
		struct task_struct *tsk;
		struct cpt_fd_image fdi;
		struct file *filp;

		read_lock(&tasklist_lock);
		tsk = find_task_by_vpid(j->pid);
		if (tsk)
			get_task_struct(tsk);
		read_unlock(&tasklist_lock);
		if (!tsk)
			return -EINVAL;

		err = rst_get_object(CPT_OBJ_FILEDESC, j->fdi, &fdi, ctx);
		if (err) {
			put_task_struct(tsk);
			return err;
		}

		if (fdi.cpt_fd >= tsk->files->fdt->max_fds) BUG();
		if (tsk->files->fdt->fd[fdi.cpt_fd] ||
		    FD_ISSET(fdi.cpt_fd, tsk->files->fdt->open_fds)) {
			eprintk_ctx("doing filejob %Ld: fd is busy\n", j->fdi);
			put_task_struct(tsk);
			return -EBUSY;
		}

		filp = rst_file(fdi.cpt_file, fdi.cpt_fd, ctx);
		if (IS_ERR(filp)) {
			eprintk_ctx("rst_do_filejobs: 1: %ld %Lu\n", PTR_ERR(filp), (unsigned long long)fdi.cpt_file);
			put_task_struct(tsk);
			return PTR_ERR(filp);
		}
		if (fdi.cpt_fd >= tsk->files->fdt->max_fds) BUG();
		tsk->files->fdt->fd[fdi.cpt_fd] = filp;
		FD_SET(fdi.cpt_fd, tsk->files->fdt->open_fds);
		if (fdi.cpt_flags&CPT_FD_FLAG_CLOSEEXEC)
			FD_SET(fdi.cpt_fd, tsk->files->fdt->close_on_exec);

		dprintk_ctx("filejob %Ld done\n", j->fdi);

		put_task_struct(tsk);
		ctx->filejob_queue = j->next;
		kfree(j);
	}
	return 0;
}

void rst_flush_filejobs(cpt_context_t *ctx)
{
	struct filejob *j;

	while ((j = ctx->filejob_queue) != NULL) {
		ctx->filejob_queue = j->next;
		kfree(j);
	}
}

int rst_fs_complete(struct cpt_task_image *ti, struct cpt_context *ctx)
{
	struct fs_struct *f = current->fs;
	cpt_object_t *obj;

	if (ti->cpt_fs == CPT_NULL) {
		exit_fs(current);
		return 0;
	}

	obj = lookup_cpt_obj_bypos(CPT_OBJ_FS, ti->cpt_fs, ctx);
	if (obj) {
		if (obj->o_obj != f) {
			exit_fs(current);
			f = obj->o_obj;
			write_lock(&f->lock);
			f->users++;
			write_unlock(&f->lock);
			current->fs = f;
		}
		return 0;
	}

	/* Do _not_ restore root. Image contains absolute pathnames.
	 * So, we fix it in context of rst process.
	 */

	obj = cpt_object_add(CPT_OBJ_FS, f, ctx);
	if (obj)
		cpt_obj_setpos(obj, ti->cpt_fs, ctx);

	return 0;
}

int cpt_get_dentry(struct dentry **dp, struct vfsmount **mp,
		   loff_t *pos, struct cpt_context *ctx)
{
	struct cpt_file_image fi;
	struct file * file;
	int err;

	err = rst_get_object(CPT_OBJ_FILE, *pos, &fi, ctx);
	if (err)
		return err;

	file = rst_file(*pos, -2, ctx);
	if (IS_ERR(file)) {
		if (PTR_ERR(file) == -EINVAL && S_ISLNK(fi.cpt_i_mode)) {
			/* One special case: inotify on symlink */
			struct nameidata nd;
			__u8 *name = NULL;

			if (fi.cpt_next > fi.cpt_hdrlen)
				name = rst_get_name(*pos + sizeof(fi), ctx);
			if (!name) {
				eprintk_ctx("can't get name for file\n");
				return -EINVAL;
			}
			if ((err = path_lookup(name, 0, &nd)) != 0) {
				eprintk_ctx("path_lookup %s: %d\n", name, err);
				rst_put_name(name, ctx);
				return -EINVAL;
			}
			*dp = nd.path.dentry;
			*mp = nd.path.mnt;
			*pos += fi.cpt_next;
			rst_put_name(name, ctx);
			return 0;
		}
		return PTR_ERR(file);
	}

	*dp = dget(file->f_dentry);
	*mp = mntget(file->f_vfsmnt);
	*pos += fi.cpt_next;
	fput(file);
	return 0;
}

static void __set_fs_root(struct fs_struct *fs, struct vfsmount *mnt,
			  struct dentry *dentry)
{
	struct dentry *old_root;
	struct vfsmount *old_rootmnt;
	write_lock(&fs->lock);
	old_root = fs->root.dentry;
	old_rootmnt = fs->root.mnt;
	fs->root.mnt = mnt;
	fs->root.dentry = dentry;
	write_unlock(&fs->lock);
	if (old_root) {
		dput(old_root);
		mntput(old_rootmnt);
	}
}

static void __set_fs_pwd(struct fs_struct *fs, struct vfsmount *mnt,
			 struct dentry *dentry)
{
	struct dentry *old_pwd;
	struct vfsmount *old_pwdmnt;

	write_lock(&fs->lock);
	old_pwd = fs->pwd.dentry;
	old_pwdmnt = fs->pwd.mnt;
	fs->pwd.mnt = mnt;
	fs->pwd.dentry = dentry;
	write_unlock(&fs->lock);

	if (old_pwd) {
		dput(old_pwd);
		mntput(old_pwdmnt);
	}
}


int rst_restore_fs(struct cpt_context *ctx)
{
	loff_t pos;
	cpt_object_t *obj;
	int err = 0;

	for_each_object(obj, CPT_OBJ_FS) {
		struct cpt_fs_struct_image fi;
		struct fs_struct *fs = obj->o_obj;
		int i;
		struct dentry *d[3];
		struct vfsmount *m[3];

		err = rst_get_object(CPT_OBJ_FS, obj->o_pos, &fi, ctx);
		if (err)
			return err;

		fs->umask = fi.cpt_umask;

		pos = obj->o_pos + fi.cpt_hdrlen;
		d[0] = d[1] = d[2] = NULL;
		m[0] = m[1] = m[2] = NULL;
		i = 0;
		while (pos < obj->o_pos + fi.cpt_next && i<3) {
			err = cpt_get_dentry(d+i, m+i, &pos, ctx);
			if (err) {
				eprintk_ctx("cannot get_dir: %d", err);
				for (--i; i >= 0; i--) {
					if (d[i])
						dput(d[i]);
					if (m[i])
						mntput(m[i]);
				}
				return err;
			}
			i++;
		}
		if (d[0])
			__set_fs_root(fs, m[0], d[0]);
		if (d[1])
			__set_fs_pwd(fs, m[1], d[1]);
		if (d[2])
			wprintk_ctx("altroot arrived...\n");
	}
	return err;
}

int do_one_mount(char *mntpnt, char *mnttype, char *mntbind,
		 unsigned long flags, unsigned long mnt_flags,
		 struct cpt_context *ctx)
{
	int err;

	if (mntbind && (strcmp(mntbind, "/") == 0 || strcmp(mntbind, "") == 0))
		mntbind = NULL;

	if (mntbind)
		flags |= MS_BIND;
	/* Join per-mountpoint flags with global flags */
	if (mnt_flags & MNT_NOSUID)
		flags |= MS_NOSUID;
	if (mnt_flags & MNT_NODEV)
		flags |= MS_NODEV;
	if (mnt_flags & MNT_NOEXEC)
		flags |= MS_NOEXEC;

	err = sc_mount(mntbind, mntpnt, mnttype, flags);
	if (err < 0) {
		eprintk_ctx("%d mounting %s %s %08lx\n", err, mntpnt, mnttype, flags);
		return err;
	}
	return 0;
}

static int undumptmpfs(void *arg)
{
	int i;
	int *pfd = arg;
	int fd1, fd2, err;
	char *argv[] = { "tar", "x", "-C", "/", "-S", NULL };

	if (pfd[0] != 0)
		sc_dup2(pfd[0], 0);

	set_fs(KERNEL_DS);
	fd1 = sc_open("/dev/null", O_WRONLY, 0);
	fd2 = sc_open("/dev/null", O_WRONLY, 0);
try:
	if (fd1 < 0 || fd2 < 0) {
		if (fd1 == -ENOENT && fd2 == -ENOENT) {
			err = sc_mknod("/dev/null", S_IFCHR|0666,
					new_encode_dev((MEM_MAJOR<<MINORBITS)|3));
			if (err < 0) {
				eprintk("can't create /dev/null: %d\n", err);
				module_put(THIS_MODULE);
				return 255 << 8;
			}
			fd1 = sc_open("/dev/null", O_WRONLY, 0666);
			fd2 = sc_open("/dev/null", O_WRONLY, 0666);
			sc_unlink("/dev/null");
			goto try;
		}
		eprintk("can not open /dev/null for tar: %d %d\n", fd1, fd2);
		module_put(THIS_MODULE);
		return 255 << 8;
	}
	if (fd1 != 1)
		sc_dup2(fd1, 1);
	if (fd2 != 2)
		sc_dup2(fd2, 2);

	for (i = 3; i < current->files->fdt->max_fds; i++)
		sc_close(i);

	module_put(THIS_MODULE);

	i = sc_execve("/bin/tar", argv, NULL);
	eprintk("failed to exec /bin/tar: %d\n", i);
	return 255 << 8;
}

static int rst_restore_tmpfs(loff_t *pos, struct cpt_context * ctx)
{
	int err;
	int pfd[2];
	struct file *f;
	struct cpt_object_hdr v;
	int n;
	loff_t end;
	int pid;
	int status;
	mm_segment_t oldfs;
	sigset_t ignore, blocked;

	err = rst_get_object(CPT_OBJ_NAME, *pos, &v, ctx);
	if (err < 0)
		return err;

	err = sc_pipe(pfd);
	if (err < 0)
		return err;
	ignore.sig[0] = CPT_SIG_IGNORE_MASK;
	sigprocmask(SIG_BLOCK, &ignore, &blocked);
	pid = err = local_kernel_thread(undumptmpfs, (void*)pfd, SIGCHLD, 0);
	if (err < 0) {
		eprintk_ctx("tmpfs local_kernel_thread: %d\n", err);
		goto out;
	}
	f = fget(pfd[1]);
	sc_close(pfd[1]);
	sc_close(pfd[0]);

	ctx->file->f_pos = *pos + v.cpt_hdrlen;
	end = *pos + v.cpt_next;
	*pos += v.cpt_next;
	do {
		char buf[16];

		n = end - ctx->file->f_pos;
		if (n > sizeof(buf))
			n = sizeof(buf);

		if (ctx->read(buf, n, ctx))
			break;
		oldfs = get_fs(); set_fs(KERNEL_DS);
		f->f_op->write(f, buf, n, &f->f_pos);
		set_fs(oldfs);
	} while (ctx->file->f_pos < end);

	fput(f);

	oldfs = get_fs(); set_fs(KERNEL_DS);
	if ((err = sc_waitx(pid, 0, &status)) < 0)
		eprintk_ctx("wait4: %d\n", err);
	else if ((status & 0x7f) == 0) {
		err = (status & 0xff00) >> 8;
		if (err != 0) {
			eprintk_ctx("tar exited with %d\n", err);
			err = -EINVAL;
		}
	} else {
		eprintk_ctx("tar terminated\n");
		err = -EINVAL;
	}
	set_fs(oldfs);
	sigprocmask(SIG_SETMASK, &blocked, NULL);

	return err;

out:
	if (pfd[1] >= 0)
		sc_close(pfd[1]);
	if (pfd[0] >= 0)
		sc_close(pfd[0]);
	sigprocmask(SIG_SETMASK, &blocked, NULL);
	return err;
}

int check_ext_mount(char *mntpnt, char *mnttype, struct cpt_context *ctx)
{
	struct mnt_namespace *n;
	struct list_head *p;
	struct vfsmount *t;
	char *path, *path_buf;
	int ret;

	n = current->nsproxy->mnt_ns;
	ret = -ENOENT;
	path_buf = cpt_get_buf(ctx);
	down_read(&namespace_sem);
	list_for_each(p, &n->list) {
		struct path pt;
		t = list_entry(p, struct vfsmount, mnt_list);
		pt.dentry = t->mnt_root;
		pt.mnt = t;
		path = d_path(&pt, path_buf, PAGE_SIZE);
		if (IS_ERR(path))
			continue;
		if (!strcmp(path, mntpnt) &&
		    !strcmp(t->mnt_sb->s_type->name, mnttype)) {
			ret = 0;
			break;
		}
	}
	up_read(&namespace_sem);
	__cpt_release_buf(ctx);
	return ret;
}

int restore_one_vfsmount(struct cpt_vfsmount_image *mi, loff_t pos, struct cpt_context *ctx)
{
	int err;
	loff_t endpos;

	endpos = pos + mi->cpt_next;
	pos += mi->cpt_hdrlen;

	while (pos < endpos) {
		char *mntdev;
		char *mntpnt;
		char *mnttype;
		char *mntbind;

		mntdev = __rst_get_name(&pos, ctx);
		mntpnt = __rst_get_name(&pos, ctx);
		mnttype = __rst_get_name(&pos, ctx);
		mntbind = NULL;
		if (mi->cpt_mntflags & CPT_MNT_BIND)
			mntbind = __rst_get_name(&pos, ctx);
		err = -EINVAL;
		if (mnttype && mntpnt) {
			err = 0;
			if (!(mi->cpt_mntflags & CPT_MNT_EXT) &&
			    strcmp(mntpnt, "/")) {
				err = do_one_mount(mntpnt, mnttype, mntbind,
						   mi->cpt_flags,
						   mi->cpt_mntflags, ctx);
				if (!err &&
				    strcmp(mnttype, "tmpfs") == 0 &&
				    !(mi->cpt_mntflags & (CPT_MNT_BIND)))
					    err = rst_restore_tmpfs(&pos, ctx);
			} else if (mi->cpt_mntflags & CPT_MNT_EXT) {
				err = check_ext_mount(mntpnt, mnttype, ctx);
				if (err)
					eprintk_ctx("mount point is missing: %s\n", mntpnt);
			}
		}
		if (mntdev)
			rst_put_name(mntdev, ctx);
		if (mntpnt)
			rst_put_name(mntpnt, ctx);
		if (mnttype)
			rst_put_name(mnttype, ctx);
		if (mntbind)
			rst_put_name(mntbind, ctx);
		if (err)
			return err;
	}
	return 0;
}

int restore_one_namespace(loff_t pos, loff_t endpos, struct cpt_context *ctx)
{
	int err;
	struct cpt_vfsmount_image mi;

	while (pos < endpos) {
		err = rst_get_object(CPT_OBJ_VFSMOUNT, pos, &mi, ctx);
		if (err)
			return err;
		err = restore_one_vfsmount(&mi, pos, ctx);
		if (err)
			return err;
		pos += mi.cpt_next;
	}
	return 0;
}

int rst_root_namespace(struct cpt_context *ctx)
{
	int err;
	loff_t sec = ctx->sections[CPT_SECT_NAMESPACE];
	loff_t endsec;
	struct cpt_section_hdr h;
	struct cpt_object_hdr sbuf;
	int done = 0;

	if (sec == CPT_NULL)
		return 0;

	err = ctx->pread(&h, sizeof(h), ctx, sec);
	if (err)
		return err;
	if (h.cpt_section != CPT_SECT_NAMESPACE || h.cpt_hdrlen < sizeof(h))
		return -EINVAL;

	endsec = sec + h.cpt_next;
	sec += h.cpt_hdrlen;
	while (sec < endsec) {
		err = rst_get_object(CPT_OBJ_NAMESPACE, sec, &sbuf, ctx);
		if (err)
			return err;
		if (done) {
			eprintk_ctx("multiple namespaces are not supported\n");
			break;
		}
		done++;
		err = restore_one_namespace(sec+sbuf.cpt_hdrlen, sec+sbuf.cpt_next, ctx);
		if (err)
			return err;
		sec += sbuf.cpt_next;
	}

	return 0;
}

int rst_stray_files(struct cpt_context *ctx)
{
	int err = 0;
	loff_t sec = ctx->sections[CPT_SECT_FILES];
	loff_t endsec;
	struct cpt_section_hdr h;

	if (sec == CPT_NULL)
		return 0;

	err = ctx->pread(&h, sizeof(h), ctx, sec);
	if (err)
		return err;
	if (h.cpt_section != CPT_SECT_FILES || h.cpt_hdrlen < sizeof(h))
		return -EINVAL;

	endsec = sec + h.cpt_next;
	sec += h.cpt_hdrlen;
	while (sec < endsec) {
		struct cpt_object_hdr sbuf;
		cpt_object_t *obj;

		err = _rst_get_object(CPT_OBJ_FILE, sec, &sbuf, sizeof(sbuf), ctx);
		if (err)
			break;

		obj = lookup_cpt_obj_bypos(CPT_OBJ_FILE, sec, ctx);
		if (!obj) {
			struct file *file;

			dprintk_ctx("stray file %Ld\n", sec);

			file = rst_sysv_shm_itself(sec, ctx);

			if (IS_ERR(file)) {
				eprintk_ctx("rst_stray_files: %ld\n", PTR_ERR(file));
				return PTR_ERR(file);
			} else {
				fput(file);
			}
		}
		sec += sbuf.cpt_next;
	}

	return err;
}
