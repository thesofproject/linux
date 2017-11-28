// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Pinecone Inc.
 *
 * redirect fs API from remote to the kernel.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <fs/internal.h>

/* start from 3 because 0, 1, 2 is reserved for stdin, stdout and stderr */
#define RPMSG_HOSTFS_ID_START		3

/* need exactly match the definitions from REMOTE include/dirent.h: */

#define HOSTFS_DTYPE_FILE		BIT(0)
#define HOSTFS_DTYPE_CHR		BIT(1)
#define HOSTFS_DTYPE_BLK		BIT(2)
#define HOSTFS_DTYPE_DIRECTORY		BIT(3)

/* need exactly match the definitions from REMOTE's include/sys/stat.h: */

#define HOSTFS_S_IFIFO			0x0000
#define HOSTFS_S_IFCHR			0x0800
#define HOSTFS_S_IFDIR			0x1000
#define HOSTFS_S_IFBLK			0x1800
#define HOSTFS_S_IFREG			0x2000
#define HOSTFS_S_IFSOCK			0x4000
#define HOSTFS_S_IFLNK			0x8000

/* need exactly match the definitions from REMOTE's include/fcntl.h: */

#define HOSTFS_O_RDONLY			BIT(0)
#define HOSTFS_O_WRONLY			BIT(1)
#define HOSTFS_O_CREAT			BIT(2)
#define HOSTFS_O_EXCL			BIT(3)
#define HOSTFS_O_APPEND			BIT(4)
#define HOSTFS_O_TRUNC			BIT(5)
#define HOSTFS_O_NONBLOCK		BIT(6)
#define HOSTFS_O_SYNC			BIT(7)
#define HOSTFS_O_BINARY			BIT(8)

/* need exactly match the definition from REMOTE's include/sys/statfs.h: */

struct hostfs_statfs {
	u32				f_type;
	u32				f_namelen;
	u32				f_bsize;
	s32				f_blocks;
	s32				f_bfree;
	s32				f_bavail;
	s32				f_files;
	s32				f_ffree;
};

/* need exactly match the definition from REMOTE's include/sys/stat.h: */

struct hostfs_stat {
	u32				st_mode;
	s32				st_size;
	s16				st_blksize;
	u32				st_blocks;
	u32				st_atim;
	u32				st_mtim;
	u32				st_ctim;
};

/* need exactly match the definition from REMOTE's fs/hostfs/hostfs_rpmsg.h: */

#define RPMSG_HOSTFS_OPEN		1
#define RPMSG_HOSTFS_CLOSE		2
#define RPMSG_HOSTFS_READ		3
#define RPMSG_HOSTFS_WRITE		4
#define RPMSG_HOSTFS_LSEEK		5
#define RPMSG_HOSTFS_IOCTL		6
#define RPMSG_HOSTFS_SYNC		7
#define RPMSG_HOSTFS_DUP		8
#define RPMSG_HOSTFS_FSTAT		9
#define RPMSG_HOSTFS_FTRUNCATE		10
#define RPMSG_HOSTFS_OPENDIR		11
#define RPMSG_HOSTFS_READDIR		12
#define RPMSG_HOSTFS_REWINDDIR		13
#define RPMSG_HOSTFS_CLOSEDIR		14
#define RPMSG_HOSTFS_STATFS		15
#define RPMSG_HOSTFS_UNLINK		16
#define RPMSG_HOSTFS_MKDIR		17
#define RPMSG_HOSTFS_RMDIR		18
#define RPMSG_HOSTFS_RENAME		19
#define RPMSG_HOSTFS_STAT		20

struct rpmsg_hostfs_header {
	u32				command;
	s32				result;
	u64				cookie;
} __packed;

struct rpmsg_hostfs_open {
	struct rpmsg_hostfs_header	header;
	s32				flags;
	s32				mode;
	char				pathname[0];
} __packed;

struct rpmsg_hostfs_close {
	struct rpmsg_hostfs_header	header;
	s32				fd;
} __packed;

struct rpmsg_hostfs_read {
	struct rpmsg_hostfs_header	header;
	s32				fd;
	u32				count;
	char				buf[0];
} __packed;

#define rpmsg_hostfs_write		rpmsg_hostfs_read

struct rpmsg_hostfs_lseek {
	struct rpmsg_hostfs_header	header;
	s32				fd;
	s32				whence;
	s32				offset;
} __packed;

struct rpmsg_hostfs_ioctl {
	struct rpmsg_hostfs_header	header;
	s32				fd;
	s32				request;
	s32				arg;
} __packed;

#define rpmsg_hostfs_sync		rpmsg_hostfs_close
#define rpmsg_hostfs_dup		rpmsg_hostfs_close

struct rpmsg_hostfs_fstat {
	struct rpmsg_hostfs_header	header;
	s32				fd;
	u32				reserved;
	struct hostfs_stat		buf;
} __packed;

struct rpmsg_hostfs_ftruncate {
	struct rpmsg_hostfs_header	header;
	s32				fd;
	s32				length;
} __packed;

struct rpmsg_hostfs_opendir {
	struct rpmsg_hostfs_header	header;
	char				pathname[0];
} __packed;

struct rpmsg_hostfs_readdir {
	struct rpmsg_hostfs_header	header;
	s32				fd;
	u32				type;
	char				name[0];
} __packed;

#define rpmsg_hostfs_rewinddir		rpmsg_hostfs_close
#define rpmsg_hostfs_closedir		rpmsg_hostfs_close

struct rpmsg_hostfs_statfs {
	struct rpmsg_hostfs_header	header;
	union {
		struct hostfs_statfs	buf;
		u32			reserved[16];
	};
	char				pathname[0];
} __packed;

#define rpmsg_hostfs_unlink		rpmsg_hostfs_opendir

struct rpmsg_hostfs_mkdir {
	struct rpmsg_hostfs_header	header;
	s32				mode;
	u32				reserved;
	char				pathname[0];
} __packed;

#define rpmsg_hostfs_rmdir		rpmsg_hostfs_opendir
#define rpmsg_hostfs_rename		rpmsg_hostfs_opendir

struct rpmsg_hostfs_stat {
	struct rpmsg_hostfs_header	header;
	union {
		struct hostfs_stat	buf;
		u32			reserved[16];
	};
	char				pathname[0];
} __packed;

struct rpmsg_hostfs_filldir_callback {
	struct dir_context		ctx;
	struct rpmsg_hostfs_readdir	*rsp;
	u32				namelen;
	u32				space;
};

struct rpmsg_hostfs {
	struct mutex			lock; /* protect files field */
	struct idr			files;
	struct kmem_cache		*cache;
};

static int rpmsg_hostfs_idr_alloc(struct rpmsg_hostfs *priv, void *ptr)
{
	int id;

	mutex_lock(&priv->lock);
	id = idr_alloc(&priv->files, ptr,
		       RPMSG_HOSTFS_ID_START, 0, GFP_KERNEL);
	mutex_unlock(&priv->lock);

	return id;
}

static void *rpmsg_hostfs_idr_find(struct rpmsg_hostfs *priv, int id)
{
	void *ptr;

	mutex_lock(&priv->lock);
	ptr = idr_find(&priv->files, id);
	mutex_unlock(&priv->lock);

	return ptr;
}

static void rpmsg_hostfs_idr_remove(struct rpmsg_hostfs *priv, int id)
{
	mutex_lock(&priv->lock);
	idr_remove(&priv->files, id);
	mutex_unlock(&priv->lock);
}

static int rpmsg_hostfs_open_handler(struct rpmsg_device *rpdev,
				     void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_open *msg = data;
	struct file *filp;
	int id, flags = 0;

	if (msg->flags & HOSTFS_O_RDONLY)
		flags |= O_RDONLY;
	if (msg->flags & HOSTFS_O_WRONLY)
		flags |= O_WRONLY;
	if (msg->flags & HOSTFS_O_APPEND)
		flags |= O_APPEND;
	if (msg->flags & HOSTFS_O_CREAT)
		flags |= O_CREAT;
	if (msg->flags & HOSTFS_O_EXCL)
		flags |= O_EXCL;
	if (msg->flags & HOSTFS_O_TRUNC)
		flags |= O_TRUNC;
	if (msg->flags & HOSTFS_O_NONBLOCK)
		flags |= O_NONBLOCK;

	filp = filp_open(msg->pathname, flags, msg->mode);
	if (!IS_ERR(filp)) {
		id = rpmsg_hostfs_idr_alloc(priv, filp);
		if (id < 0)
			filp_close(filp, NULL);
		msg->header.result = id;
	} else {
		msg->header.result = PTR_ERR(filp);
	}

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_close_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_close *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		rpmsg_hostfs_idr_remove(priv, msg->fd);
		ret = filp_close(filp, NULL);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_read_handler(struct rpmsg_device *rpdev,
				     void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_read *msg = data;
	struct rpmsg_hostfs_read *rsp;
	struct file *filp;
	int ret = -ENOENT;
	u32 space;

	rsp = rpmsg_get_tx_payload_buffer(rpdev->ept, &space, true);
	if (!rsp)
		return -ENOMEM;
	*rsp = *msg;

	space -= sizeof(*msg);
	if (space > msg->count)
		space = msg->count;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		void *buf;

		if (priv->cache)
			buf = kmem_cache_alloc(priv->cache, GFP_KERNEL);
		else
			buf = rsp->buf;

		ret = kernel_read(filp, buf, space, &filp->f_pos);
		if (priv->cache && buf) {
			if (ret > 0)
				memcpy(rsp->buf, buf, ret);
			kmem_cache_free(priv->cache, buf);
		}
	}

	rsp->header.result = ret;
	return rpmsg_send_nocopy(rpdev->ept,
		rsp, (ret < 0 ? 0 : ret) + sizeof(*rsp));
}

static int rpmsg_hostfs_write_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_write *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		void *buf;

		if (priv->cache) {
			buf = kmem_cache_alloc(priv->cache, GFP_KERNEL);
			if (buf)
				memcpy(buf, msg->buf, msg->count);
		} else {
			buf = msg->buf;
		}

		ret = kernel_write(filp, buf, msg->count, &filp->f_pos);
		if (priv->cache && buf)
			kmem_cache_free(priv->cache, buf);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_lseek_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_lseek *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp)
		ret = vfs_llseek(filp, msg->offset, msg->whence);

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_ioctl_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_ioctl *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp)
		ret = vfs_ioctl(filp, msg->request, msg->arg);

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_sync_handler(struct rpmsg_device *rpdev,
				     void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_sync *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp)
		ret = vfs_fsync(filp, 0);

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_dup_handler(struct rpmsg_device *rpdev,
				    void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_dup *msg = data;
	struct file *filp, *new_filp;
	int id, ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		new_filp = fget(f_dupfd(0, filp, 0));
		if (new_filp) {
			id = rpmsg_hostfs_idr_alloc(priv, new_filp);
			if (id < 0)
				filp_close(new_filp, NULL);
			ret = id;
		}
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static void
rpmsg_hostfs_stat_convert(struct kstat *hostbuf, struct hostfs_stat *buf)
{
	buf->st_mode = hostbuf->mode & 0777;

	if (hostbuf->mode & S_IFDIR)
		buf->st_mode |= HOSTFS_S_IFDIR;
	else if (hostbuf->mode & S_IFREG)
		buf->st_mode |= HOSTFS_S_IFREG;
	else if (hostbuf->mode & S_IFCHR)
		buf->st_mode |= HOSTFS_S_IFCHR;
	else if (hostbuf->mode & S_IFBLK)
		buf->st_mode |= HOSTFS_S_IFBLK;
	else if (hostbuf->mode & S_IFLNK)
		buf->st_mode |= HOSTFS_S_IFLNK;
	else  if (hostbuf->mode & S_IFIFO)
		buf->st_mode |= HOSTFS_S_IFIFO;
	else  if (hostbuf->mode & S_IFSOCK)
		buf->st_mode |= HOSTFS_S_IFSOCK;

	buf->st_size    = hostbuf->size;
	buf->st_blksize = hostbuf->blksize;
	buf->st_blocks  = hostbuf->blocks;
	buf->st_atim    = hostbuf->atime.tv_sec;
	buf->st_mtim    = hostbuf->mtime.tv_sec;
	buf->st_ctim    = hostbuf->ctime.tv_sec;
}

static int rpmsg_hostfs_fstat_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_fstat *msg = data;
	struct kstat hostbuf;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		ret = vfs_getattr(&filp->f_path, &hostbuf,
				  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
		if (ret == 0)
			rpmsg_hostfs_stat_convert(&hostbuf, &msg->buf);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int
rpmsg_hostfs_ftruncate_handler(struct rpmsg_device *rpdev,
			       void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_ftruncate *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp)
		ret = vfs_truncate(&filp->f_path, msg->length);

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int
rpmsg_hostfs_opendir_handler(struct rpmsg_device *rpdev,
			     void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_opendir *msg = data;
	struct file *filp;
	int id;

	filp = filp_open(msg->pathname, O_RDONLY | O_DIRECTORY, 0);
	if (!IS_ERR(filp)) {
		id = rpmsg_hostfs_idr_alloc(priv, filp);
		if (id < 0)
			filp_close(filp, NULL);
		msg->header.result = id;
	} else {
		msg->header.result = PTR_ERR(filp);
	}

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_filldir(struct dir_context *ctx,
				const char *name, int namelen,
				loff_t offset, u64 ino, unsigned int d_type)
{
	struct rpmsg_hostfs_filldir_callback *cb =
		container_of(ctx, struct rpmsg_hostfs_filldir_callback, ctx);
	struct rpmsg_hostfs_readdir *rsp = cb->rsp;

	if (cb->namelen)
		return 1;

	if (d_type == DT_REG)
		rsp->type = HOSTFS_DTYPE_FILE;
	else if (d_type == DT_CHR)
		rsp->type = HOSTFS_DTYPE_CHR;
	else if (d_type == DT_BLK)
		rsp->type = HOSTFS_DTYPE_BLK;
	else if (d_type == DT_DIR)
		rsp->type = HOSTFS_DTYPE_DIRECTORY;
	else
		rsp->type = 0;

	if (namelen >= cb->space)
		namelen = cb->space - 1;

	strncpy(rsp->name, name, namelen);
	rsp->name[namelen++] = 0;
	cb->namelen = namelen;

	return 0;
}

static int
rpmsg_hostfs_readdir_handler(struct rpmsg_device *rpdev,
			     void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_filldir_callback cb = {
		.ctx.actor = rpmsg_hostfs_filldir,
		.namelen   = 0,
	};
	struct rpmsg_hostfs_readdir *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	cb.rsp = rpmsg_get_tx_payload_buffer(rpdev->ept, &cb.space, true);
	if (!cb.rsp)
		return -ENOMEM;
	cb.space -= sizeof(*msg);
	*cb.rsp = *msg;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		ret = iterate_dir(filp, &cb.ctx);
		if (ret == 0 && cb.namelen == 0)
			ret = -ENOENT;
	}

	cb.rsp->header.result = ret;
	return rpmsg_send_nocopy(rpdev->ept,
		cb.rsp, sizeof(*cb.rsp) + cb.namelen);
}

static int
rpmsg_hostfs_rewinddir_handler(struct rpmsg_device *rpdev,
			       void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_rewinddir *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		ret = vfs_llseek(filp, 0, SEEK_SET);
		if (ret > 0)
			ret = -EINVAL;
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int
rpmsg_hostfs_closedir_handler(struct rpmsg_device *rpdev,
			      void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_closedir *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = rpmsg_hostfs_idr_find(priv, msg->fd);
	if (filp) {
		rpmsg_hostfs_idr_remove(priv, msg->fd);
		ret = filp_close(filp, NULL);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_statfs_handler(struct rpmsg_device *rpdev,
				       void *data, int len, void *priv, u32 src)
{
	struct rpmsg_hostfs_statfs *msg = data;
	struct hostfs_statfs *buf = &msg->buf;
	struct kstatfs hostbuf;
	struct file *filp;
	int ret;

	filp = filp_open(msg->pathname, 0, 0);
	if (!IS_ERR(filp)) {
		ret = vfs_statfs(&filp->f_path, &hostbuf);
		if (ret == 0) {
			buf->f_type    = hostbuf.f_type;
			buf->f_namelen = hostbuf.f_namelen;
			buf->f_bsize   = hostbuf.f_bsize;
			buf->f_blocks  = hostbuf.f_blocks;
			buf->f_bfree   = hostbuf.f_bfree;
			buf->f_bavail  = hostbuf.f_bavail;
			buf->f_files   = hostbuf.f_files;
			buf->f_ffree   = hostbuf.f_ffree;
		}
		filp_close(filp, NULL);
	} else {
		ret = PTR_ERR(filp);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_unlink_handler(struct rpmsg_device *rpdev,
				       void *data, int len, void *priv, u32 src)
{
	struct rpmsg_hostfs_unlink *msg = data;
	struct dentry *dentry;
	struct path parent;
	int ret;

	dentry = kern_path_locked(msg->pathname, &parent);
	if (!IS_ERR(dentry)) {
		if (d_really_is_positive(dentry))
			ret = vfs_unlink(d_inode(parent.dentry), dentry, NULL);
		else
			ret = -ENOENT;

		dput(dentry);
		inode_unlock(d_inode(parent.dentry));
		path_put(&parent);
	} else {
		ret = PTR_ERR(dentry);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_mkdir_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv, u32 src)
{
	struct rpmsg_hostfs_mkdir *msg = data;
	struct dentry *dentry;
	struct path path;
	int ret;

	dentry = kern_path_create(AT_FDCWD, msg->pathname,
				  &path, LOOKUP_DIRECTORY);
	if (!IS_ERR(dentry)) {
		ret = vfs_mkdir(d_inode(path.dentry), dentry, msg->mode);
		done_path_create(&path, dentry);
	} else {
		ret = PTR_ERR(dentry);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_rmdir_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv, u32 src)
{
	struct rpmsg_hostfs_rmdir *msg = data;
	struct dentry *dentry;
	struct path parent;
	int ret;

	dentry = kern_path_locked(msg->pathname, &parent);
	if (!IS_ERR(dentry)) {
		if (d_really_is_positive(dentry))
			ret = vfs_rmdir(d_inode(parent.dentry), dentry);
		else
			ret = -ENOENT;

		dput(dentry);
		inode_unlock(d_inode(parent.dentry));
		path_put(&parent);
	} else {
		ret = PTR_ERR(dentry);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_rename_handler(struct rpmsg_device *rpdev,
				       void *data, int len, void *priv, u32 src)
{
	struct rpmsg_hostfs_rename *msg = data;
	struct path oldpath, newpath;
	struct dentry *newdentry;
	char *oldname, *newname;
	int ret, oldlen;

	oldname = msg->pathname;
	oldlen  = (strlen(msg->pathname) + 1 + 0x7) & ~0x7;
	newname = msg->pathname + oldlen;

	ret = kern_path(oldname, 0, &oldpath);
	if (ret < 0)
		goto fail;

	if (!oldpath.dentry || !oldpath.dentry->d_parent) {
		ret = -ENOENT;
		goto fail1;
	}

	newdentry = kern_path_locked(newname, &newpath);
	if (IS_ERR(newdentry)) {
		ret = PTR_ERR(newdentry);
		goto fail1;
	}

	ret = vfs_rename(oldpath.dentry->d_parent->d_inode, oldpath.dentry,
			 d_inode(newpath.dentry), newdentry, NULL, 0);

	dput(newdentry);
	inode_unlock(d_inode(newpath.dentry));
	path_put(&newpath);
fail1:
	path_put(&oldpath);
fail:
	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_stat_handler(struct rpmsg_device *rpdev,
				     void *data, int len, void *priv, u32 src)
{
	struct rpmsg_hostfs_stat *msg = data;
	struct kstat hostbuf;
	struct file *filp;
	int ret;

	filp = filp_open(msg->pathname, 0, 0);
	if (!IS_ERR(filp)) {
		ret = vfs_getattr(&filp->f_path, &hostbuf,
				  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
		if (ret == 0)
			rpmsg_hostfs_stat_convert(&hostbuf, &msg->buf);
		filp_close(filp, NULL);
	} else {
		ret = PTR_ERR(filp);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static const rpmsg_rx_cb_t rpmsg_hostfs_handler[] = {
	[RPMSG_HOSTFS_OPEN]      = rpmsg_hostfs_open_handler,
	[RPMSG_HOSTFS_CLOSE]     = rpmsg_hostfs_close_handler,
	[RPMSG_HOSTFS_READ]      = rpmsg_hostfs_read_handler,
	[RPMSG_HOSTFS_WRITE]     = rpmsg_hostfs_write_handler,
	[RPMSG_HOSTFS_LSEEK]     = rpmsg_hostfs_lseek_handler,
	[RPMSG_HOSTFS_IOCTL]     = rpmsg_hostfs_ioctl_handler,
	[RPMSG_HOSTFS_SYNC]      = rpmsg_hostfs_sync_handler,
	[RPMSG_HOSTFS_DUP]       = rpmsg_hostfs_dup_handler,
	[RPMSG_HOSTFS_FSTAT]     = rpmsg_hostfs_fstat_handler,
	[RPMSG_HOSTFS_FTRUNCATE] = rpmsg_hostfs_ftruncate_handler,
	[RPMSG_HOSTFS_OPENDIR]   = rpmsg_hostfs_opendir_handler,
	[RPMSG_HOSTFS_READDIR]   = rpmsg_hostfs_readdir_handler,
	[RPMSG_HOSTFS_REWINDDIR] = rpmsg_hostfs_rewinddir_handler,
	[RPMSG_HOSTFS_CLOSEDIR]  = rpmsg_hostfs_closedir_handler,
	[RPMSG_HOSTFS_STATFS]    = rpmsg_hostfs_statfs_handler,
	[RPMSG_HOSTFS_UNLINK]    = rpmsg_hostfs_unlink_handler,
	[RPMSG_HOSTFS_MKDIR]     = rpmsg_hostfs_mkdir_handler,
	[RPMSG_HOSTFS_RMDIR]     = rpmsg_hostfs_rmdir_handler,
	[RPMSG_HOSTFS_RENAME]    = rpmsg_hostfs_rename_handler,
	[RPMSG_HOSTFS_STAT]      = rpmsg_hostfs_stat_handler,
};

static int rpmsg_hostfs_callback(struct rpmsg_device *rpdev,
				 void *data, int len, void *priv, u32 src)
{
	struct rpmsg_hostfs_header *header = data;
	u32 cmd = header->command;
	int ret = -EINVAL;

	if (cmd < ARRAY_SIZE(rpmsg_hostfs_handler)) {
		ret = rpmsg_hostfs_handler[cmd](rpdev, data, len, priv, src);
		if (ret < 0)
			dev_err(&rpdev->dev, "command handle error %d\n", cmd);
	}

	return ret;
}

static int rpmsg_hostfs_probe(struct rpmsg_device *rpdev)
{
	struct device_node *np = rpdev->dev.of_node;
	struct rpmsg_hostfs *priv;
	bool aligned;

	priv = devm_kzalloc(&rpdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	aligned = of_property_read_bool(np, "memory-aligned-access");
	if (!aligned) {
		/* try the parent node */
		np = of_get_parent(np);
		aligned = of_property_read_bool(np, "memory-aligned-access");
		of_node_put(np);
	}

	if (aligned) {
		int size = rpmsg_get_max_bufsize(rpdev->ept);

		priv->cache = kmem_cache_create(dev_name(&rpdev->dev),
						size, 8, 0, NULL);
		if (!priv->cache)
			return -ENOMEM;
	}

	mutex_init(&priv->lock);
	idr_init(&priv->files);
	dev_set_drvdata(&rpdev->dev, priv);

	return 0;
}

static void rpmsg_hostfs_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct file *filp;
	int fd;

	idr_for_each_entry(&priv->files, filp, fd)
		filp_close(filp, NULL);

	kmem_cache_destroy(priv->cache);
	mutex_destroy(&priv->lock);
	idr_destroy(&priv->files);
}

static const struct rpmsg_device_id rpmsg_hostfs_id_table[] = {
	{ .name = "rpmsg-hostfs" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_hostfs_id_table);

static struct rpmsg_driver rpmsg_hostfs_driver = {
	.drv = {
		.name	= "rpmsg_hostfs",
		.owner	= THIS_MODULE,
	},

	.id_table	= rpmsg_hostfs_id_table,
	.probe		= rpmsg_hostfs_probe,
	.callback	= rpmsg_hostfs_callback,
	.remove		= rpmsg_hostfs_remove,
};

module_rpmsg_driver(rpmsg_hostfs_driver);

MODULE_ALIAS("rpmsg:rpmsg_hostfs");
MODULE_AUTHOR("Guiding Li <liguiding@pinecone.net>");
MODULE_DESCRIPTION("rpmsg fs API redirection driver");
MODULE_LICENSE("GPL v2");
