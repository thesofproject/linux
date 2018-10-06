/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2017 Pinecone Inc.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/rpmsg.h>
#include <linux/statfs.h>
#include <fs/internal.h>

/* start from 3 because 0, 1, 2 is reserved for stdin, stdout and stderr */
#define RPMSG_HOSTFS_ID_START		3

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

/* These must exactly match the definitions from REMOTE include/dirent.h: */

#define HOSTFS_DTYPE_FILE		0x01
#define HOSTFS_DTYPE_CHR		0x02
#define HOSTFS_DTYPE_BLK		0x04
#define HOSTFS_DTYPE_DIRECTORY		0x08

/* These must exactly match the definitions from REMOTE include/sys/stat.h: */

#define HOSTFS_S_IFIFO			(0 << 11)
#define HOSTFS_S_IFCHR			(1 << 11)
#define HOSTFS_S_IFDIR			(2 << 11)
#define HOSTFS_S_IFBLK			(3 << 11)
#define HOSTFS_S_IFREG			(4 << 11)
#define HOSTFS_S_IFSOCK			(8 << 11)
#define HOSTFS_S_IFLNK			(1 << 15)

/* These must exactly match the definitions from REMOTE include/fcntl.h: */

#define HOSTFS_O_RDONLY			(1 << 0)  /* Open for read access (only) */
#define HOSTFS_O_WRONLY			(1 << 1)  /* Open for write access (only) */
#define HOSTFS_O_CREAT			(1 << 2)  /* Create file/sem/mq object */
#define HOSTFS_O_EXCL			(1 << 3)  /* Name must not exist when opened */
#define HOSTFS_O_APPEND			(1 << 4)  /* Keep contents, append to end */
#define HOSTFS_O_TRUNC			(1 << 5)  /* Delete contents */
#define HOSTFS_O_NONBLOCK		(1 << 6)  /* Don't wait for data */
#define HOSTFS_O_SYNC			(1 << 7)  /* Synchronize output on write */
#define HOSTFS_O_BINARY			(1 << 8)  /* Open the file in binary mode. */

/* These must exactly match the definition from REMOTE include/sys/statfs.h: */

struct hostfs_statfs {
	uint32_t			f_type;		/* Type of filesystem */
	uint32_t			f_namelen;	/* Maximum length of filenames */
	uint32_t			f_bsize;	/* Optimal block size for transfers */
	int32_t				f_blocks;	/* Total data blocks in the file system of this size */
	int32_t				f_bfree;	/* Free blocks in the file system */
	int32_t				f_bavail;	/* Free blocks avail to non-superuser */
	int32_t				f_files;	/* Total file nodes in the file system */
	int32_t				f_ffree;	/* Free file nodes in the file system */
};

/* These must exactly match the definition from REMOTE include/sys/stat.h: */

struct hostfs_stat {
	uint32_t			st_mode;	/* File type, atributes, and access mode bits */
	int32_t				st_size;	/* Size of file/directory, in bytes */
	int16_t				st_blksize;	/* Blocksize used for filesystem I/O */
	uint32_t			st_blocks;	/* Number of blocks allocated */
	uint32_t			st_atim;	/* Time of last access */
	uint32_t			st_mtim;	/* Time of last modification */
	uint32_t			st_ctim;	/* Time of last status change */
};

/* These must exactly match the definition from REMOTE fs/hostfs/hostfs_rpmsg.h: */

struct rpmsg_hostfs_header {
	uint32_t			command;
	int32_t				result;
	uint64_t			cookie;
} __packed;

struct rpmsg_hostfs_open {
	struct rpmsg_hostfs_header	header;
	int32_t				flags;
	int32_t				mode;
	char				pathname[0];
} __packed;

struct rpmsg_hostfs_close {
	struct rpmsg_hostfs_header	header;
	int32_t				fd;
} __packed;

struct rpmsg_hostfs_read {
	struct rpmsg_hostfs_header	header;
	int32_t				fd;
	uint32_t			count;
	char				buf[0];
} __packed;

#define rpmsg_hostfs_write		rpmsg_hostfs_read

struct rpmsg_hostfs_lseek {
	struct rpmsg_hostfs_header	header;
	int32_t				fd;
	int32_t				whence;
	int32_t				offset;
} __packed;

struct rpmsg_hostfs_ioctl {
	struct rpmsg_hostfs_header	header;
	int32_t				fd;
	int32_t				request;
	int32_t				arg;
} __packed;

#define rpmsg_hostfs_sync		rpmsg_hostfs_close
#define rpmsg_hostfs_dup		rpmsg_hostfs_close

struct rpmsg_hostfs_fstat {
	struct rpmsg_hostfs_header	header;
	int32_t				fd;
	uint32_t			reserved;
	struct hostfs_stat		buf;
} __packed;

struct rpmsg_hostfs_ftruncate {
	struct rpmsg_hostfs_header	header;
	int32_t				fd;
	int32_t				length;
} __packed;

struct rpmsg_hostfs_opendir {
	struct rpmsg_hostfs_header	header;
	char				pathname[0];
} __packed;

struct rpmsg_hostfs_readdir {
	struct rpmsg_hostfs_header	header;
	int32_t				fd;
	uint32_t			type;
	char				name[0];
} __packed;

#define rpmsg_hostfs_rewinddir		rpmsg_hostfs_close
#define rpmsg_hostfs_closedir		rpmsg_hostfs_close

struct rpmsg_hostfs_statfs {
	struct rpmsg_hostfs_header	header;
	union {
		struct hostfs_statfs	buf;
		uint32_t		reserved[16];
	};
	char				pathname[0];
} __packed;

#define rpmsg_hostfs_unlink		rpmsg_hostfs_opendir

struct rpmsg_hostfs_mkdir {
	struct rpmsg_hostfs_header	header;
	int32_t				mode;
	uint32_t			reserved;
	char				pathname[0];
} __packed;

#define rpmsg_hostfs_rmdir		rpmsg_hostfs_opendir
#define rpmsg_hostfs_rename		rpmsg_hostfs_opendir

struct rpmsg_hostfs_stat {
	struct rpmsg_hostfs_header	header;
	union {
		struct hostfs_stat	buf;
		uint32_t		reserved[16];
	};
	char				pathname[0];
} __packed;

struct rpmsg_hostfs_filldir_callback {
	struct dir_context		ctx;
	struct rpmsg_hostfs_readdir	*rsp;
	uint32_t			namelen;
	uint32_t			space;
};

struct rpmsg_hostfs {
	struct idr			files;
	bool				need_copy;
};

static int rpmsg_hostfs_open_handler(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_open *msg = data;
	int id, flags = 0;
	struct file *filp;

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
	if (IS_ERR(filp))
		msg->header.result = PTR_ERR(filp);
	else {
		id = idr_alloc(&priv->files, filp,
				RPMSG_HOSTFS_ID_START, 0, GFP_KERNEL);
		if (id < 0)
			filp_close(filp, NULL);

		msg->header.result = id;
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

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		idr_remove(&priv->files, msg->fd);
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
	uint32_t space;
	int ret = -ENOENT;

	rsp = rpmsg_get_tx_payload_buffer(rpdev->ept, &space, true);
	if (!rsp)
		return -ENOMEM;

	*rsp = *msg;

	space -= sizeof(*msg);
	if (space > msg->count)
		space = msg->count;

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		char tbuf[space] __aligned(8);
		char *tmp;

		if (priv->need_copy)
			tmp = tbuf;
		else
			tmp = rsp->buf;

		ret = kernel_read(filp, tmp, space, &filp->f_pos);
		if (ret > 0) {
			if (priv->need_copy)
				memcpy(rsp->buf, tbuf, ret);
		}
	}

	rsp->header.result = ret;
	return rpmsg_send_nocopy(rpdev->ept, rsp, (ret < 0 ? 0 : ret) + sizeof(*rsp));
}

static int rpmsg_hostfs_write_handler(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_write *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		char tbuf[msg->count] __aligned(8);
		char *tmp;

		if (priv->need_copy) {
			tmp = tbuf;
			memcpy(tbuf, msg->buf, msg->count);
		} else
			tmp = msg->buf;

		ret = kernel_write(filp, tmp, msg->count, &filp->f_pos);
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

	filp = idr_find(&priv->files, msg->fd);
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

	filp = idr_find(&priv->files, msg->fd);
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

	filp = idr_find(&priv->files, msg->fd);
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

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		new_filp = fget(f_dupfd(0, filp, 0));
		if (new_filp) {
			id = idr_alloc(&priv->files, new_filp,
					RPMSG_HOSTFS_ID_START, 0, GFP_KERNEL);
			if (id < 0)
				filp_close(new_filp, NULL);

			ret = id;
		}
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static void rpmsg_hostfs_stat_convert(struct kstat *hostbuf, struct hostfs_stat *buf)
{
	/* Map the return values */

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
	struct file *filp;
	struct kstat hostbuf;
	int ret = -ENOENT;

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		ret = vfs_getattr(&filp->f_path, &hostbuf,
				  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
		if (ret == 0)
			rpmsg_hostfs_stat_convert(&hostbuf, &msg->buf);
	}

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_ftruncate_handler(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_ftruncate *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = idr_find(&priv->files, msg->fd);
	if (filp)
		ret = vfs_truncate(&filp->f_path, msg->length);

	msg->header.result = ret;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_opendir_handler(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_opendir *msg = data;
	struct file *filp;
	int id;

	filp = filp_open(msg->pathname, O_RDONLY|O_DIRECTORY, 0);
	if (IS_ERR(filp))
		msg->header.result = PTR_ERR(filp);
	else {
		id = idr_alloc(&priv->files, filp,
				RPMSG_HOSTFS_ID_START, 0, GFP_KERNEL);
		if (id < 0)
			filp_close(filp, NULL);

		msg->header.result = id;
	}

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_filldir(struct dir_context *ctx, const char *name,
		int namelen, loff_t offset, u64 ino, unsigned int d_type)
{
	struct rpmsg_hostfs_filldir_callback *cb =
		container_of(ctx, struct rpmsg_hostfs_filldir_callback, ctx);
	struct rpmsg_hostfs_readdir *rsp = cb->rsp;
	int i;

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

	for (i = 0; i < namelen; i++)
		rsp->name[i] = name[i];
	rsp->name[namelen++] = 0;
	cb->namelen = namelen;

	return 0;
}

static int rpmsg_hostfs_readdir_handler(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_readdir *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	struct rpmsg_hostfs_filldir_callback cb = {
		.ctx.actor = rpmsg_hostfs_filldir,
		.namelen   = 0,
	};

	cb.rsp = rpmsg_get_tx_payload_buffer(rpdev->ept, &cb.space, true);
	if (!cb.rsp)
		return -ENOMEM;
	cb.space -= sizeof(*msg);
	*cb.rsp = *msg;

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		ret = iterate_dir(filp, &cb.ctx);
		if (ret == 0 && cb.namelen == 0)
			ret = -ENOENT;
	}

	cb.rsp->header.result = ret;
	return rpmsg_send_nocopy(rpdev->ept, cb.rsp, sizeof(*cb.rsp) + cb.namelen);
}

static int rpmsg_hostfs_rewinddir_handler(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_rewinddir *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		ret = vfs_llseek(filp, 0, SEEK_SET);
		if (ret > 0)
			ret = -EINVAL;
	}

	msg->header.result = ret;

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_hostfs_closedir_handler(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_hostfs *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_hostfs_closedir *msg = data;
	struct file *filp;
	int ret = -ENOENT;

	filp = idr_find(&priv->files, msg->fd);
	if (filp) {
		idr_remove(&priv->files, msg->fd);
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
	if (IS_ERR(filp))
		ret = PTR_ERR(filp);
	else {
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
	if (IS_ERR(dentry))
		ret = PTR_ERR(dentry);
	else {
		if (d_really_is_positive(dentry))
			ret = vfs_unlink(d_inode(parent.dentry), dentry, NULL);
		else
			ret = -ENOENT;

		dput(dentry);
		inode_unlock(d_inode(parent.dentry));
		path_put(&parent);
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

	dentry = kern_path_create(AT_FDCWD, msg->pathname, &path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		ret = PTR_ERR(dentry);
	else {
		ret = vfs_mkdir(d_inode(path.dentry), dentry, msg->mode);
		done_path_create(&path, dentry);
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
	if (IS_ERR(dentry))
		ret = PTR_ERR(dentry);
	else {
		if (d_really_is_positive(dentry))
			ret = vfs_rmdir(d_inode(parent.dentry), dentry);
		else
			ret = -ENOENT;

		dput(dentry);
		inode_unlock(d_inode(parent.dentry));
		path_put(&parent);
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

	ret = vfs_rename(oldpath.dentry->d_parent->d_inode,
			oldpath.dentry,
			d_inode(newpath.dentry),
			newdentry,
			NULL,
			0);

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
	if (IS_ERR(filp))
		ret = PTR_ERR(filp);
	else {
		ret = vfs_getattr(&filp->f_path, &hostbuf,
				  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
		if (ret == 0)
			rpmsg_hostfs_stat_convert(&hostbuf, &msg->buf);

		filp_close(filp, NULL);
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
	uint32_t command = header->command;
	int ret = -EINVAL;

	if (command < ARRAY_SIZE(rpmsg_hostfs_handler)) {
		ret = rpmsg_hostfs_handler[command](rpdev, data, len, priv, src);
		if (ret)
			dev_err(&rpdev->dev, "command handle error %d\n", command);
	}

	return ret;
}

static int rpmsg_hostfs_probe(struct rpmsg_device *rpdev)
{
	struct device_node *np = rpdev->dev.of_node;
	struct rpmsg_hostfs *priv;

	priv = devm_kzalloc(&rpdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->need_copy = of_property_read_bool(np, "memory-aligned-access");
	if (!priv->need_copy) {
		/* try the parent node */
		np = of_get_parent(np);
		priv->need_copy = of_property_read_bool(np, "memory-aligned-access");
		of_node_put(np);
	}

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

module_driver(rpmsg_hostfs_driver,
		register_rpmsg_driver,
		unregister_rpmsg_driver);

MODULE_ALIAS("rpmsg:rpmsg_hostfs");
MODULE_AUTHOR("Guiding Li <liguiding@pinecone.net>");
MODULE_DESCRIPTION("rpmsg fs API redirection driver");
MODULE_LICENSE("GPL v2");
