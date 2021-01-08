#ifndef PREETTY_PRINT_H_
#define PREETTY_PRINT_H_

# include "lfs.h"

// 为每个数据结构提供pretty_print函数
#define pretty_print_struct(st, field, format, typecast) \
  printf("    " #field " = " #format "\n", typecast st->field)

void pretty_print_fi (struct fuse_file_info *fi)
{
	printf("PRETTY PRINT FUSE FILE INFO:\n");
    
    /** Open flags.  Available in open() and release() */
    //	int flags;
	pretty_print_struct(fi, flags, %d, );

    /** In case of a write operation indicates if this was caused by a writepage */
    //	int writepage;
	pretty_print_struct(fi, writepage, %d, );

    /** Can be filled in by open, to use direct I/O on this file. */
    //	unsigned int direct_io;
	pretty_print_struct(fi, direct_io, %u, );

    /** Can be filled in by open. It signals the kernel that any currently
	 cached file data (ie., data that the filesystem provided the last
	 time the file was open) need not be invalidated. Has no effect
	 when set in other contexts (in particular it does nothing when set by opendir()). */
    //	unsigned int keep_cache : 1;
	pretty_print_struct(fi, keep_cache, %u, );

    /** Indicates a flush operation. Set in flush operation, also maybe set
	 in highlevel lock operation and lowlevel release operation. */
    //	unsigned int flush : 1;
	pretty_print_struct(fi, flush, %u, );

    /** Can be filled in by open, to indicate that the file is not seekable. */
    //	unsigned int nonseekable : 1;
	pretty_print_struct(fi, nonseekable, %u, );

	/** Indicates that flock locks for this file should be released. If set,
	 lock_owner shall contain a valid value. May only be set in ->release(). */
    //	unsigned int flock_release : 1;
	pretty_print_struct(fi, flock_release, %u, );

	/** Can be filled in by opendir. It signals the kernel to enable caching
	 of entries returned by readdir().  Has no effect when set in other
	 contexts (in particular it does nothing when set by open()). */
    //	unsigned int cache_readdir : 1;u
	pretty_print_struct(fi, cache_readdir, %u, );

    /** File handle.  May be filled in by filesystem in open().
        Available in all other file operations */
    //	uint64_t fh;
	pretty_print_struct(fi, fh, 0x%016lx,  );
	
    /** Lock owner id.  Available in locking operations and flush */
    //  uint64_t lock_owner;
	pretty_print_struct(fi, lock_owner, 0x%016lx, );

	/** Requested poll events.  Available in ->poll.  Only set on kernels
	 which support it.  If unsupported, this field is set to zero. */
    //  uint64_t poll_events;
	pretty_print_struct(fi, poll_events, %u, );
}


void pretty_print_conn(struct fuse_conn_info *conn)
{
	printf("PRETTY PRINT FUSE CONN:\n");
	/** Major version of the protocol (read-only) */
	// unsigned proto_major;
    pretty_print_struct(conn, proto_major, %u, );

	/** Minor version of the protocol (read-only) */
	// unsigned proto_minor;
	pretty_print_struct(conn, proto_minor, %u, );

	/** Maximum size of the write buffer */
	// unsigned max_write;
	pretty_print_struct(conn, max_write, %u, );

	/** Maximum size of read requests. A value of zero indicates no limit.
	 However, even if the filesystem does not specify a limit, the maximum
	 size of read requests will still be limited by the kernel.
	 For the time being, the maximum size of read requests must be set both
	 here *and* passed to fuse_session_new() using the ``-o max_read=<n>`` mount option. 
	 At some point in the future, specifying the mount option will no longer be necessary.
	 */
	// unsigned max_read;
    pretty_print_struct(conn, max_read, %u, );

	/** Maximum readahead */
	// unsigned max_readahead;
	pretty_print_struct(conn, max_readahead, %u, );

	/** Capability flags that the kernel supports (read-only) */
	// unsigned capable;
	pretty_print_struct(conn, capable, %u, );

	/** Capability flags that the filesystem wants to enable.
	  libfuse attempts to initialize this field with
	  reasonable default values before calling the init() handler. */
	// unsigned want;
	pretty_print_struct(conn, want, %u, );

	/** Maximum number of pending "background" requests. A
	 * background request is any type of request for which the
	 * total number is not limited by other means. As of kernel
	 * 4.8, only two types of requests fall into this category:
	 *
	 *   1. Read-ahead requests
	 *   2. Asynchronous direct I/O requests
	 *
	 * Read-ahead requests are generated (if max_readahead is
	 * non-zero) by the kernel to preemptively fill its caches
	 * when it anticipates that userspace will soon read more
	 * data.
	 *
	 * Asynchronous direct I/O requests are generated if
	 * FUSE_CAP_ASYNC_DIO is enabled and userspace submits a large
	 * direct I/O request. In this case the kernel will internally
	 * split it up into multiple smaller requests and submit them
	 * to the filesystem concurrently.
	 *
	 * Note that the following requests are *not* background
	 * requests: writeback requests (limited by the kernel's
	 * flusher algorithm), regular (i.e., synchronous and
	 * buffered) userspace read/write requests (limited to one per
	 * thread), asynchronous read requests (Linux's io_submit(2)
	 * call actually blocks, so these are also limited to one per
	 * thread). */
	// unsigned max_background;
	pretty_print_struct(conn, max_background, %u, );

	/** Kernel congestion threshold parameter. If the number of pending
	 * background requests exceeds this number, the FUSE kernel module will
	 * mark the filesystem as "congested". This instructs the kernel to
	 * expect that queued requests will take some time to complete, and to
	 * adjust its algorithms accordingly (e.g. by putting a waiting thread
	 * to sleep instead of using a busy-loop). */
	// unsigned congestion_threshold;
	pretty_print_struct(conn, congestion_threshold, %u, );

	/** When FUSE_CAP_WRITEBACK_CACHE is enabled, the kernel is responsible
	 * for updating mtime and ctime when write requests are received. The
	 * updated values are passed to the filesystem with setattr() requests.
	 * However, if the filesystem does not support the full resolution of
	 * the kernel timestamps (nanoseconds), the mtime and ctime values used
	 * by kernel and filesystem will differ (and result in an apparent
	 * change of times after a cache flush).
	 * To prevent this problem, this variable can be used to inform the
	 * kernel about the timestamp granularity supported by the file-system.
	 * The value should be power of 10.  The default is 1, i.e. full
	 * nano-second resolution. Filesystems supporting only second resolution
	 * should set this to 1000000000.
	 */
	// unsigned time_gran;
    pretty_print_struct(conn, time_gran, %u, );
}

void pretty_print_config(struct fuse_config *cfg)
{   
	printf("PRETTY PRINT FUSE CONFIG:\n");
	/* If `set_gid` is non-zero, the st_gid attribute of each file is overwritten with the value of `gid`.*/
	// int set_gid;
    pretty_print_struct(cfg, set_gid, %d, );
	// unsigned int gid;
	pretty_print_struct(cfg, gid, %u, );

	/* If `set_uid` is non-zero, the st_uid attribute of each file is overwritten with the value of `uid`. */
	// int set_uid;
	pretty_print_struct(cfg, set_uid, %d, );
	// unsigned int uid;
	pretty_print_struct(cfg, uid, %u, );

	/* If `set_mode` is non-zero, the any permissions bits set in `umask` are unset in the st_mode attribute of each file. */
	// int set_mode;
	pretty_print_struct(cfg, set_mode, %d, );
	// unsigned int umask;
	pretty_print_struct(cfg, umask, %u, );

	/* The timeout in seconds for which name lookups will be cached. */
	// double entry_timeout;
	pretty_print_struct(cfg, entry_timeout, %f, );

	/* The timeout in seconds for which a negative lookup will be cached. This means,
	* that if file did not exist (lookup returned ENOENT), the lookup will only be redone after the timeout,
	* and the file/directory will be assumed to not exist until then. A value of zero means that negative lookups are not cached. */
	// double negative_timeout;
	pretty_print_struct(cfg, negative_timeout, %f, );

	/* The timeout in seconds for which file/directory attributes (as returned by e.g. the `getattr` handler) are cached. */
	// double attr_timeout;
	pretty_print_struct(cfg, attr_timeout, %f, );

	/* Allow requests to be interrupted */
	//int intr;
	pretty_print_struct(cfg, intr, %d, );

	/* Specify which signal number to send to the filesystem when a request is interrupted.  The default is hardcoded to USR1. */
	//int intr_signal;
	pretty_print_struct(cfg, intr_signal, %d, );

	/* Normally, FUSE assigns inodes to paths only for as long as the kernel is aware of them.
	* With this option inodes are instead remembered for at least this many seconds. 
	* This will require more memory, but may be necessary when using applications that make use of inode numbers.
	* A number of -1 means that inodes will be remembered for the entire life-time of the file-system process. */
	//int remember;
	pretty_print_struct(cfg, remember, %d, );

	/* The default behavior is that if an open file is deleted, the file is renamed to a hidden file (.fuse_hiddenXXX),
	* and only removed when the file is finally released.  This relieves the filesystem implementation of having to deal with this problem.
	* This option disables the hiding behavior, and files are removed immediately in an unlink operation 
	*(or in a rename operation which overwrites an existing file). It is recommended that you not use the hard_remove option.
	* When hard_remove is set, the following libc functions fail on unlinked files (returning errno of ENOENT):
	* read(2), write(2), fsync(2), close(2), f*xattr(2), ftruncate(2), fstat(2), fchmod(2), fchown(2) */
	//int hard_remove;
	pretty_print_struct(cfg, hard_remove, %d, );

	/** Honor the st_ino field in the functions getattr() and fill_dir().
	* This value is used to fill in the st_ino field in the stat(2), lstat(2), fstat(2) functions and the d_ino field in the readdir(2) function.
	* The filesystem does not have to guarantee uniqueness, however some applications rely on this value being unique for the whole filesystem.
	* Note that this does *not* affect the inode that libfuse  and the kernel use internally (also called the "nodeid"). */
	//int use_ino;
	pretty_print_struct(cfg, use_ino, %d, );

	/** If use_ino option is not given, still try to fill in the d_ino field in readdir(2).
	* If the name was previously looked up, and is still in the cache, the inode number found there will be used.
	* Otherwise it will be set to -1. If use_ino option is given, this option is ignored. */
	//int readdir_ino;
	pretty_print_struct(cfg, readdir_ino, %d, );

	/** This option disables the use of page cache (file content cache) in the kernel for this filesystem.
	* This has several affects:
	* 1. Each read(2) or write(2) system call will initiate one or more read or write operations, data will not be cached in the kernel.
	* 2. The return value of the read() and write() system calls will correspond to the return values of the read and write operations.
	* This is useful for example if the file size is not known in advance (before reading it).
	* Internally, enabling this option causes fuse to set the `direct_io` field of `struct fuse_file_info` - overwriting any value that was put there by the file system.
    */
	//int direct_io;
	pretty_print_struct(cfg, direct_io, %d, );

	/** This option disables flushing the cache of the file contents on every open(2).  This should only be enabled on filesystems where the file data is never changed externally (not through the mounted FUSE filesystem).
	* Thus it is not suitable for network filesystems and other intermediate filesystems.
	* if this option is not specified (and neither direct_io) data is still cached after the open(2), so a read(2) system call will not always initiate a read operation.
	* Internally, enabling this option causes fuse to set the `keep_cache` field of `struct fuse_file_info` - overwriting any value that was put there by the file system.
	*/
	//int kernel_cache;
	pretty_print_struct(cfg, kernel_cache, %d, );

	/** This option is an alternative to `kernel_cache`. Instead of unconditionally keeping cached data, the cached data is invalidated on open(2) if if the modification time or the size of the file has changed since it was last opened. 这个选项是' kernel_cache '的另一种选择。如果修改时间或文件的大小自上次打开后发生了变化，缓存数据将在打开时失效，而不是无条件地保存缓存数据。*/
	//int auto_cache;
	pretty_print_struct(cfg, auto_cache, %d, );

	/**The timeout in seconds for which file attributes are cached for the purpose of checking if auto_cache should flush the file data on open. 用于检查auto_cache是否应该在打开时刷新文件数据的缓存文件属性的超时秒数。*/
	//int ac_attr_timeout_set;
	pretty_print_struct(cfg, ac_attr_timeout_set, %d, );
	//double ac_attr_timeout;
	pretty_print_struct(cfg, ac_attr_timeout, %f, );

	/** If this option is given the file-system handlers for the following operations will not receive path information: 如果给出此选项，用于以下操作的文件系统处理程序将不会接收路径信息
read, write, flush, release, fallocate, fsync, readdir, releasedir, fsyncdir, lock, ioctl and poll.
For the truncate, getattr, chmod, chown and utimens operations the path will be provided only if the struct fuse_file_info argument is NULL. 对于truncate、getattr、chmod、chown和utimens操作，只有当struct fuse_file_info参数为NULL时，才会提供路径。
	 */
	//int nullpath_ok;
	pretty_print_struct(cfg, nullpath_ok, %d, );

	/**The remaining options are used by libfuse internally and should not be touched. 其余选项由libfuse在内部使用，不应该使用。*/
	//int show_help;
	pretty_print_struct(cfg, show_help, %d, );
	//char *modules;
	pretty_print_struct(cfg, modules, %s, );
	//int debug;
	pretty_print_struct(cfg, debug, %d, );
};

void pretty_print_stat(struct stat *stat)
{   
	printf("PRETTY PRINT LIBICONV SRCLIB SYS STAT:\n");
    //dev_t st_dev;
    pretty_print_struct(stat, st_dev, %ld, );

    //ino_t st_ino;
    pretty_print_struct(stat, st_ino, %ld, );	
    //mode_t st_mode;
    pretty_print_struct(stat, st_mode, %d, );	
    //nlink_t st_nlink;
    pretty_print_struct(stat, st_nlink, %ld, );

    /* uid_t is not defined by default on native Windows.  */
	/*
	# if 0
    uid_t st_uid;
    # else 
    short st_uid;
    # endif
	*/
	pretty_print_struct(stat, st_uid, %d, );

    /* gid_t is not defined by default on native Windows.  */
	/*
	# if 0
    gid_t st_gid;
    # else
    short st_gid;
    # endif
	*/
    pretty_print_struct(stat, st_gid, %d, );

    // dev_t st_rdev;
	pretty_print_struct(stat, st_rdev, %ld, );

    // off_t st_size;
	pretty_print_struct(stat, st_size, %ld, );

    /*
    # if 0
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    # endif
    */
	pretty_print_struct(stat, st_blksize, %ld, );
	pretty_print_struct(stat, st_blocks, %ld, );

    /*
    # if 0
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    # else
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
    # endif
	*/
	pretty_print_struct(stat, st_atime, %ld, );
	pretty_print_struct(stat, st_mtime, %ld, );
	pretty_print_struct(stat, st_ctime, %ld, );
};

void pretty_print_lfs_file_inode_hash(struct lfs_file_inode_hash *inode_in_hash)
{
	printf("PRETTY PRINT LFS FILE INODE HASH:\n");
    
	pretty_print_struct(inode_in_hash, f_name, %s, );

	pretty_print_struct(inode_in_hash, inode_num, %u, );

	pretty_print_struct(inode_in_hash, f_size, %u, );

	pretty_print_struct(inode_in_hash, owner_num, %u, );

    pretty_print_struct(inode_in_hash, group_num, %u, );
	
	pretty_print_struct(inode_in_hash, is_dir, %u, );
};
#endif