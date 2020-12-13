#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>

#include <ctype.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include <stdint.h>
#include "uthash.h"  // 这个是一个开源项目，直接拿来用的

// 文件名最大长度
#define MAXNAMELEN	128
// 按总体要求中提出的块大小为1KB
#define BLKSIZE		1024
// 每个段的容量（字节数）
#define SEG_SIZE	6*BLKSIZE
// 每段最大的块数
#define MAX_SEG_BLKS	6
// 最大的段数
#define MAX_NUM_SEG     8
// 每个文件最大的块数
#define MAX_BLKS_FOR_FILE	500
// 最大的索引结点数
#define MAX_INODES	1024
// 一个段中dead的块超过该阈值时就需要进行清理
#define THRESHOLD	(MAX_SEG_BLKS / 2)

// 文件所有者名称最大长度
#define MAXOWNERLEN 20
#define MAXGROUPLEN 20


// 文件直接数据块的磁盘地址
struct direct_blk {
	int32_t seg_num;  // 段号
	int32_t blk_num;  // 段中块号
};

// 索引结点
// 每个文件对应一个索引结点
struct inode {
	uint32_t ino;	// 索引结点编号
	uint32_t size;  // 
	struct direct_blk direct[MAX_BLKS_FOR_FILE];  // 文件的所有直接数据块的磁盘地址（段号、段中块号）
};

// 段摘要
struct segsum {
	int32_t inode_num;	// inode number of the file
	int32_t logical_blk;	// logical block number of the file
};

// 所有文件的基本信息的哈希表
struct file_inode_hash {
    char f_name[MAXNAMELEN];  // 文件名最大长度
    uint32_t inode_num;  // 索引结点编号
	uint32_t f_size;  // 文件大小
    UT_hash_handle hh;
	char f_owner[MAXOWNERLEN];  // 文件所有者最大长度
	char f_group[MAXGROUPLEN];  // 文件所有者所属组最大长度
};

// 已用
struct inode_map_entry {
	int32_t seg_num;
	int32_t blk_num;
};

// 已用
struct lfs_global_info {
	// hash table mapping filename to file_inode_hash structure
	struct file_inode_hash *fih;

	// 当前段的描述符号
	char  *cur_seg_buf;

	// 当前段的当前块
	uint32_t cur_seg_blk;

	// 创建的索引结点数量
	uint64_t n_inode;

	// segment number of next free segment
	int32_t log_head;

	// the rbtree for inode map entrees
	struct inode_map_entry ino_map[MAX_INODES];

	// segment bitmap
	uint16_t seg_bitmap[MAX_NUM_SEG];

    // 用于决定段是否需要清理的阈值
	uint32_t threshold;

	// the file descriptor of the file representing the disk log
	int fd;
};

char* expected_open = NULL;
struct lfs_global_info *li;

/* 骨架里的代码 begin */
struct private_state {
    char *rootdir;
};

// 由参数中的路径生成完整路径
static void lfs_fullpath(char fpath[PATH_MAX], const char *path)
{
    struct private_state * echofs_private_state = (struct private_state *) fuse_get_context()->private_data;
    strcpy(fpath, echofs_private_state->rootdir);
    strncat(fpath, path, PATH_MAX); // ridiculously long paths will break here
}
/* 骨架里的代码 end */

// 这些代码间有递归调用，所以把函数声明全放这里
void copy_segmentdata_to_log(int fd, char * buf, size_t count, off_t offset );
void read_from_log(int seg_num, int block_num, char *buf, int size, int blk_offset);
void lfs_clean();
int get_next_free_segment();
int num_of_free_segments();
int clean_cost(int segment_num, char *ssbuf);

// 内存中缓存段的当前块是最后一块时，将缓存的段写入磁盘
void copy_segmentdata_to_log(int fd, char * buf, size_t count, off_t offset )
{
	size_t ret;
	// ss指向段摘要块的第一个摘要条目
	struct segsum *ss = (struct segsum*)(li->cur_seg_buf);
	// 如果内存中缓存段的当前块已是最后一块，则写入磁盘
	if( li->cur_seg_blk == MAX_SEG_BLKS -1)
	{
		// 将缓冲段的所有内容写入磁盘
		ret = pwrite(fd, buf, count, offset);
		assert(ret == count);

		// 更新段标志，表明指定段中已写入内容
		li->seg_bitmap[li->log_head] = 0;
        
		// 获取下一个空闲段的段号
		li->log_head = get_next_free_segment();
	
	    // 空闲段数不足时执行清理
		if(num_of_free_segments() <= 5)
		{
			li->cur_seg_blk = 1;
			lfs_clean();
		}

		li->cur_seg_blk = 0;

		// 将缓存段的内容清0
		memset((void*)li->cur_seg_buf, 0, SEG_SIZE);

		// 重置段摘要块标志
		ss[0].inode_num = -1;
		ss[0].logical_blk = -1;
	}
	return;
}

// 从磁盘中指定位置读出指定数量的字节
void read_from_log(int seg_num, int block_num, char *buf, int size, int blk_offset)
{
    int offset;
    offset = seg_num * SEG_SIZE + block_num * BLKSIZE + BLKSIZE + blk_offset;
    pread(li->fd, buf, size, offset);
    return;
}

// 清理所有的段
void lfs_clean()
{
	// 一个段大小的空间
	char *new_buf = malloc(SEG_SIZE);
	// 一个块大小的空间
	char *ssbuf = malloc(BLKSIZE);

	int ino, blk, i, j, db;

	// 对所有的段
	for(i=0; i < MAX_NUM_SEG; i++)
	{
	    if(li->seg_bitmap[i] == 0)  // 若段非空
	    {
			db = clean_cost(i, ssbuf);  // db为该段中dead的块数，ssbuf中为该段的段摘要块内容

			if(db >= li->threshold)  // 超过阈值，需要清理
			{
				// 待清理段的内容都读到new_buf中
				read_from_log(i, 0, new_buf, SEG_SIZE, 0);

				// live blocks are to be copied into this segment	
				struct segsum *ss = (struct segsum *) li->cur_seg_buf; // 当前缓存段的段摘要，live的块要复制到此段中

				// segment to be cleaned
				struct segsum *clean_ss = (struct segsum *) ssbuf ; // 当前待清理段的段摘要
				for(j = 1; j < MAX_SEG_BLKS; )
				{
					// 对当前待清理段中的块逐个处理（从1开始，已跳过段摘要块）
					ino = clean_ss[j].inode_num;
					blk = clean_ss[j].logical_blk;

					if( ino != -1)  // 若该块不是dead的（执行clean_cost的时候把dead块的inode_num设置为-1的）
					{
						// inode of the block not in memory
						if( li->ino_map[ino].seg_num != li->log_head) // 若该块的索引结点不在内存中（该块索引结点所在的段不是当前缓存的段）
						{
							// 把该块的索引结点块读到当前缓存段的当前块
							// 因为移动块后要更改索引结点中对应条目的内容
							read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, BLKSIZE, 0);

							// 更新inode map中此索引结点的条目
							li->ino_map[ino].seg_num = li->log_head; 
							li->ino_map[ino].blk_num = li->cur_seg_blk;
						
							// 更新段摘要中此块的条目
							ss[li->cur_seg_blk].inode_num = ino;  
							ss[li->cur_seg_blk].logical_blk = -1;  // 此块为索引结点块 
						
						    // 内存中缓存段的当前块是最后一块（函数内部会判断）时，将缓存的段写入磁盘
							copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + BLKSIZE);

							// 当前缓存段的块号加1
							li->cur_seg_blk++;
						}
						// check if the current block in clean segment is an inode	
						if( blk == -1) 
						{
							j++;
							// as inode is copied, continue with the process
							continue;  
						}
						// 索引结点，可能是原先就在缓存段中的，也可能是刚读入的
						struct inode *id = (struct inode *) (li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE); 

						// 把待清理段（之前已读入到new_buf中）中当前块的内容移动到缓存段的当前块
						memmove(li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, new_buf + j*BLKSIZE, BLKSIZE); 

						// 更新索引结点中对此块的记录条目
						id->direct[blk].seg_num = li->log_head; 
						id->direct[blk].blk_num = li->cur_seg_blk;
						
						// 更新缓存段的段摘要
						ss[li->cur_seg_blk].inode_num = ino; 
						ss[li->cur_seg_blk].logical_blk = blk; 

						// 内存中缓存段的当前块是最后一块（函数内部会判断）时，将缓存的段写入磁盘
						copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE,li->log_head * SEG_SIZE + BLKSIZE);

                        // 缓存段当前块号加1
						li->cur_seg_blk++;
					}
					// 处理待清理段中下一个块
					j++;
				}	
				// 已完成清理的段标志为空闲
				li->seg_bitmap[i] = 1;
		    }
		}
	}
	return;
}

// 寻找下一个标志为空闲的段，若找不到，则返回-1
int get_next_free_segment()
{
    int i,j;
	j = li->log_head + 1;
    for(i = 0; i < MAX_NUM_SEG ; i++, j++) 
    {
		if( j == MAX_NUM_SEG )
		{
			j = 0;
		}
        
		if(li->seg_bitmap[j] == 1)
        {
            return j;
        }
    }
    return -1;
}

// 统计空闲的段数
int num_of_free_segments()
{
	int i, count = 0;
	for(i = 0; i < MAX_NUM_SEG; i++)
	{
		if(li->seg_bitmap[i] == 1)
		{
			count++;
		}
	}
	return count;
}

// 返回指定段中dead的块数，ssbuf中为该段摘要块内容
int clean_cost(int segment_num, char *ssbuf)
{
	int ino, blk, j, deadblock = 0; 
	struct inode *i;
	char *ibuf;

	ibuf = malloc(BLKSIZE);
	int segno = segment_num;
    
	// 读入当前段的段摘要块
	read_from_log(segno, 0, ssbuf, BLKSIZE, 0);
	struct segsum *ss = (struct segsum *) ssbuf;

	for(j = 1; j < MAX_SEG_BLKS; j++)
	{
		// 该段每一个块的摘要
		ino = ss[j].inode_num;
        blk = ss[j].logical_blk;

		// represents its an inode
		if(blk == -1) // 该块为索引结点块
		{
			//if segment number or block number in ifile dont match
			if((li->ino_map[ino].seg_num != segno) || (li->ino_map[ino].blk_num != j)) 
			{
				// mark as dead block
				ss[j].inode_num = -1;  
				deadblock++;
			}
		}
		else
		{
			// 该块为数据块（该文件的第blk个直接数据块）

			// 读出该块所属文件的索引结点
			read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, ibuf, BLKSIZE, 0);
			i = (struct inode *) ibuf;
			
			// if block address in inode do not match
			if((i->direct[blk].seg_num != segno) || (i->direct[blk].blk_num != j)) 
			{
				// mark as a deadblock
				ss[j].inode_num = -1;  
				ss[j].logical_blk = -1;
				deadblock++;
			}
		}
	}
	free(ibuf);
	return deadblock;
}

// 从路径中提取文件名部分
char* get_filename(const char *path)
{
	int i=0;
	while (*(path+i) != '\0') i++;
	while (*(path+i) != '/') i--;
	i++;
	return path+i;
}

// 核心功能代码
void *lfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	printf("\nWXH LFS INIT\n");
    return (struct private_state *) fuse_get_context()->private_data;
}

// 根据自定义文件系统中指定文件或目录的信息，填充一个文件状态结构类型的数据，提供给FUSE
static int lfs_getattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS GETATTR\npath=\"%s\", statbuf=0x%08lx, fi=0x%08lx\n", fpath, (unsigned long)statbuf, (unsigned long)fi);

    // 文件操作
	int res = 0;
	struct file_inode_hash *s;
	char * fname = get_filename(path);  //提取出文件名
	
	// 要根据我们的文件系统中该文件的信息，填充fuse中的文件状态结构类型数据
	memset(statbuf, 0, sizeof(struct stat));  // 初始化为全0

	if (strcmp(path, "/") == 0)
	{
		statbuf->st_mode = S_IFDIR | 0755;  // 标志出，该文件是一个目录
		statbuf->st_nlink = 2;  // 目录的硬链接数量为什么是2呢？是指.和..吗？
		res = 0;
		return res;
	} else {
		for (s=li->fih; s!=NULL; s=s->hh.next)
		{
			if(strcmp(fname, s->f_name) == 0)  // 文件名相等
			{		
				statbuf->st_mode = S_IFREG | 0755;  // 标志出，该文件是一个普通文件
				statbuf->st_nlink = 1;
				res = 0;
				statbuf->st_size = s->f_size;
				return res;
			}
		}
	}
	res = -ENOENT;  // 出错，不存在
	return res;
}

int lfs_opendir(const char *path, struct fuse_file_info *fi)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS OPENDIR\npath=\"%s\"\tfi=0x%08lx\n", fpath, (unsigned long)fi);
    return 0;
}

static int lfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS READDIR\npath=\"%s\", buf=0x%08lx, filler=0x%08lx, offset=%ld, fi=0x%08lx, flags=%d\n", fpath, (unsigned long)buf, (unsigned long)filler, offset, (unsigned long)fi, flags);
	(void) offset;
	(void) fi;
	struct file_inode_hash *s;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0, FUSE_FILL_DIR_PLUS);
	filler(buf, "..", NULL, 0, FUSE_FILL_DIR_PLUS);
	for (s=li->fih; s!=NULL; s=s->hh.next)
	{
		filler(buf, s->f_name, NULL, 0, FUSE_FILL_DIR_PLUS);
	}

	return 0;
}

int lfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS RELEASEDIR\npath=\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);
    return 0;
}

// file open operation
int lfs_open(const char *path, struct fuse_file_info *fi) 
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS OPEN\npath\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);

	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih, get_filename(path), s);

	if(strcmp(s->f_name, path) != 0)
		return 0;
	else
		return -1;
}

int lfs_read(const char *path, char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS READ\npath=\"%s\", buf=0x%08lx, count=%ld, offset=%ld, fi=0x%08lx\n", fpath, (unsigned long)buf, (unsigned long)count, offset, (unsigned long)fi);
	struct inode *i;
	uint32_t ino;
	int pos,n,blk;
	char *ibuf = malloc(BLKSIZE);

	// 在哈希表中查找指定的文件
	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih, get_filename(path), s);

	if( s == NULL)
	{
		// 该文件不存在
		return -ENOENT;
	}
	else
	{
		// 找到该文件，查到该文件的索引结点号
		ino = s->inode_num;
	}
	
	memset(buf, 0, count);  // 按待读出的字节数准备好缓冲区

	if(li->ino_map[ino].seg_num != li->log_head)
	{
		// 索引结点不在当前缓存的段中，则单独读出该索引结点块，放在ibuf中
		read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, ibuf, BLKSIZE, 0);
		i = (struct inode *) ibuf;
	}
	else
	{
		// 索引结点在当前缓存的段中，找到该块
		i = (struct inode *) (li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE);
	}

	if(offset  >=  i->size)
	{
		// 起始位置超出了文件总长度，则返回0
		return 0;
	}

    if (i->size - offset < count)
	{
		// 起始位置到文件尾的字节数小于准备读的字节数，则以实际能读的字节数为准
		count = i->size - offset;
	}

	for(pos = offset; pos < offset + count; )  // pos是文件中的偏移位置
	{
		n = BLKSIZE - pos % BLKSIZE < offset + count - pos ? BLKSIZE - pos % BLKSIZE : offset + count - pos;  // 根据这个偏移量，从一个块中可以读出的字节数
		blk = pos/BLKSIZE;  // 得到当前待读取字节所在的逻辑块号，即文件中的第几个块
			
		if(i->direct[blk].seg_num == li->log_head)
		{
            // 如果这个数据块在当前缓存段中，则读入n个字节
			memmove(buf,  li->cur_seg_buf + i->direct[blk].blk_num * BLKSIZE+ pos % BLKSIZE, n);
		}
		else{
			// 如果这个数据块不在当前缓存段中，则从磁盘读入该块中的n个字节
			read_from_log(i->direct[blk].seg_num, i->direct[blk].blk_num, buf, n, pos % BLKSIZE);
		}

		pos += n;
		buf += n;
	}

	return count;
}

int lfs_flush(const char *path, struct fuse_file_info *fi)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS FLUSH\npath=\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);
    return 0;
}

int lfs_release(const char *path, struct fuse_file_info *fi)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS RELEASE\npath=\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);
    return 0;
}

int lfs_link(const char *path, const char *newpath)
{
	// 自己写
    // 硬链接的意思是一个档案可以有多个名称
    char fpath[PATH_MAX];
	char fnewpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	lfs_fullpath(fnewpath, newpath);
    printf("\nWXH LFS LINK\npath=\"%s\", newpath=\"%s\"\n", fpath, fnewpath);
	
	// 在哈希表中查找指定的文件
	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih, get_filename(path), s);

	if( s == NULL)
	{
		// 该文件不存在
		return -ENOENT;
	}
	else
	{
		// 找到
		struct file_inode_hash *s1 = (struct file_inode_hash*)malloc(sizeof(struct file_inode_hash));
		s1->f_size = s->f_size;
		s1->inode_num = s->inode_num;
		strcpy(s1->f_name, get_filename(newpath));  // 新的名称

		HASH_ADD_STR(li->fih, f_name, s1);
	}

    return 0;
}

int lfs_rename(const char *path, const char *newpath, unsigned int flags)
{
	// TODO
    char fpath[PATH_MAX];
	char fnewpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	lfs_fullpath(fnewpath, newpath);
    printf("\nWXH LFS RENAME\nfpath=\"%s\", newpath=\"%s\"\n", fpath, fnewpath);

    // 在哈希表中查找指定的文件
	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih, get_filename(path), s);

	if( s == NULL)
	{
		// 该文件不存在
		return -ENOENT;
	}
	else
	{
		// 找到
		struct file_inode_hash *s1 = (struct file_inode_hash*)malloc(sizeof(struct file_inode_hash));
		s1->f_size = s->f_size;
		s1->inode_num = s->inode_num;
		strcpy(s1->f_name, get_filename(newpath));  // 新的名称

		HASH_ADD_STR(li->fih, f_name, s1);

		HASH_DEL(li->fih, s);
	}

    return 0;
}

int lfs_access(const char *path, int mask)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS ACCESS\npath=\"%s\", mask=0%o\n", fpath, mask);
    return 0;
}

int lfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS MKNOD\npath=\"%s\", mode=0%3o\tdev=%ld\n", fpath, mode, dev);
    return 0;
}

int lfs_unlink(const char *path)
{
	char fpath[PATH_MAX];
	lfs_fullpath(fpath, path);
	printf("\nWXH LFS UNLINK\npath=\"%s\"\n", fpath);

    // 从哈希表查找待删除文件
	int ino;
	struct file_inode_hash *s;
	HASH_FIND_STR(li->fih, get_filename(path), s);
	
	if(s == NULL)  // 未找到该文件
	{
		return -1;
	}

    // 找到该文件的索引结点，清为空
	ino = s->inode_num;
	li->ino_map[ino].seg_num = -1;
	li->ino_map[ino].blk_num = -1;

    // 从哈希表中删除该文件结点
	HASH_DEL(li->fih, s);

	return 0;
}

int lfs_write(const char *path, const char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
	char fpath[PATH_MAX];
	lfs_fullpath(fpath, path);
	printf("\nWXH LFS WRITE\npath=\"%s\", buf=0x%08lx, count=%ld, offset=%ld, fi=0x%08lx\n", fpath, (unsigned long)buf, count, offset, (unsigned long)fi);

	// 在哈希表中查找文件
	struct file_inode_hash *s;
    uint32_t ino;
	HASH_FIND_STR(li->fih, get_filename(path), s);

	if( s == NULL)
	{
		// 待写入的文件不存在
		return -ENOENT;
	}
	else
	{
		// 找到待写入文件的索引结点
		ino = s->inode_num;
	}

    // 将待写入文件的索引结点块的内容读到ibuf中
	char *ibuf = malloc(BLKSIZE);
	if(li->ino_map[ino].seg_num != li->log_head) 
	{
		read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, ibuf, BLKSIZE, 0);
	}
	else
	{
		memmove(ibuf,  li->cur_seg_buf  + li->ino_map[ino].blk_num * BLKSIZE, BLKSIZE);
	}
	
	// 完成buf中内容的写入
	struct inode *i = (struct inode *) ibuf;	
	struct segsum *ss = (struct segsum *) li->cur_seg_buf;  // 指向当前缓冲段的段摘要块
	int blk, segno;
	uint32_t n;
	for(int pos = offset; pos < offset + count;)
	{
		// 能写入当前块的字节数，可能写满一个块，也可能只写若干字节
		n = BLKSIZE - pos % BLKSIZE < offset + count - pos ? BLKSIZE - pos % BLKSIZE : offset + count - pos;
		blk = pos/BLKSIZE; // 逻辑块号，即文件中的第几个块

		if( pos + n > i->size)
		{
			// 从这个地方看，这个写操作应该是改写而非插入，即会覆盖掉原来的内容
			i->size = pos + n; 	// update file size accordingly. 
		}

		// 这个逻辑块若尚未存在，则得到-1；若已存在，则得到此块所在段号
		segno = i->direct[blk].seg_num;
	
		if(segno != -1)  // 逻辑块已存在
		{	
			if( pos % BLKSIZE != 0 )  // pos为0的话也就相当于整块写入了
			{
				if( segno == li->log_head) // 逻辑块已经在内存了，完成buf中内容写入到块
				{				
					memmove( li->cur_seg_buf + i->direct[blk].blk_num * BLKSIZE, buf, n); 
					pos += n; // 更新写入的起始点（相对于文件中）
					buf += n;
					continue;  // 跳过本次循环后续操作，进入下一次循环
  				}
				else
				{
					// 直接从磁盘读入整块的内容到缓存段的当前块
					read_from_log(i->direct[blk].seg_num, i->direct[blk].blk_num, li->cur_seg_buf + li->cur_seg_blk*BLKSIZE, BLKSIZE, 0); 
				}
			}
		}
		
		// 在索引结点中更新此逻辑块的信息（ibuf中）
		i->direct[blk].seg_num  = li->log_head;
		i->direct[blk].blk_num = li->cur_seg_blk; 

		// 将buf中内容写入此块
		memmove( li->cur_seg_buf + (li->cur_seg_blk ) * BLKSIZE, buf, n); 

		// 更新段摘要块中内容
		ss[li->cur_seg_blk].inode_num = ino;
		ss[li->cur_seg_blk].logical_blk = blk; 

		// 如果此块是当前缓存段中的最后一块（函数内会判断），则写入磁盘
		copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, (li->log_head * SEG_SIZE + BLKSIZE));

		li->cur_seg_blk++; 		
		pos += n; // update pos.
		buf += n;
	}

    // 用ibuf中的内容更新此文件的索引结点块
	if(li->ino_map[ino].seg_num == li->log_head)
	{
		// 该索引结点块已在当前缓存段中
		memmove(li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE, ibuf, BLKSIZE);
	}
	else
	{
		// 将ibuf内容复制到缓存段的当前块中
		memmove(li->cur_seg_buf + li->cur_seg_blk * BLKSIZE, ibuf, BLKSIZE);

		// 更新inode map
		li->ino_map[ino].seg_num = li->log_head; 
		li->ino_map[ino].blk_num = li->cur_seg_blk;

		// 更新段摘要块
		ss[li->cur_seg_blk].inode_num = ino;
		ss[li->cur_seg_blk].logical_blk = -1;

	    // 如果此块是当前缓存段中的最后一块（函数内会判断），则写入磁盘
		copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE,(li->log_head * SEG_SIZE + BLKSIZE));

        // 缓存段当前块加1
		li->cur_seg_blk ++; 	
	}
	
	// 更新哈希表中对应文件的长度信息;
    if(s->f_size != i->size) 
	{
		struct file_inode_hash *s1 = (struct file_inode_hash*)malloc(sizeof(struct file_inode_hash));
		s1->f_size = i->size;
		s1->inode_num = s->inode_num;
		strcpy(s1->f_name, s->f_name);

		HASH_DEL(li->fih,s);
		HASH_ADD_STR(li->fih,f_name,s1);
	}
	
	return count;
}

int lfs_truncate(const char *path, off_t newsize, struct fuse_file_info *fi)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH TRUNCATE\npath=\"%s\", newsize=%ld, fi=0x%08lx\n", fpath, newsize, (unsigned long)fi);
    return 0;
}

int lfs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH FSYNC\npath=\"%s\", datasync=%d, fi=0x%08lx\n", fpath, datasync, (unsigned long)fi);
    return 0;
}

int lfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS CHOWN\npath=\"%s\", uid=%d, gid=%d, fi=0x%08lx\n", fpath, uid, gid, (unsigned long)fi);
    return 0;
}

int lfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    // TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS CHMOD\npath=\"%s\", mode=0%03o, fi=0x%08lx\n", fpath, mode, (unsigned long)fi);
    return 0;
}

// New file creation
int lfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS CREATE\npath=\"%s\", mode=0%03o, fi=0x%08lx\n", fpath, mode, (unsigned long)fi);

	expected_open = path;
	struct segsum *ss;
	int j;
	struct file_inode_hash *s;

	HASH_FIND_STR(li->fih, get_filename(path), s);  // 根据文件名，找到该文件对应的file_inode_hash结点，s指向该结点

	if(s != NULL)
	{
		// 指定名称的文件已存在，直接返回
		return 0;
	} else {
		// 在当前段中生成新文件的inode块并初始化
		struct inode *i = (struct inode *)(li->cur_seg_buf + (li->cur_seg_blk * BLKSIZE));  // 在当前段的当前块新建一个索引结点
		i->ino = li->n_inode++;
		i->size = 0;
		for(j = 0; j <= MAX_BLKS_FOR_FILE; j++)
		{
			i->direct[j].seg_num = -1;
			i->direct[j].blk_num = -1;
		}

		// add the newly created inode to for given file into the hash table
		s = (struct file_inode_hash*)malloc(sizeof(struct file_inode_hash));
		strcpy(s->f_name, get_filename(path));
		s->inode_num = i->ino;
		s->f_size = i->size;
		HASH_ADD_STR(li->fih, f_name, s);

		// 记录该索引结点的段号和块号
		li->ino_map[i->ino].seg_num = li->log_head;
		li->ino_map[i->ino].blk_num = li->cur_seg_blk; 
		
		// 更新段摘要中内容
		ss = (struct segsum *)(li->cur_seg_buf);
		ss[li->cur_seg_blk].inode_num = i->ino;
		ss[li->cur_seg_blk].logical_blk = -1;

		// if the in-memory segment is full, write the data to disk
        copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + BLKSIZE); 

		// 当前段的当前块号加1		
		li->cur_seg_blk++;
	}

	return 0;
}

int lfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS GETXATTR\npath=\"%s\", name=0x%08lx, value=0x%08lx, size=%ld\n", fpath, (unsigned long)name, (unsigned long)value, size);
	
	return 0;
}

static const struct fuse_operations lfs_oper = {
  .init = lfs_init,
  .getattr = lfs_getattr,
  .readdir = lfs_readdir,
  .open = lfs_open,
  .read = lfs_read,
  .write = lfs_write,
  .unlink = lfs_unlink,

  .create = lfs_create,

  /*自己写*/
  .mknod = lfs_mknod,
  .rename = lfs_rename,
  .link = lfs_link,
  .chmod = lfs_chmod,
  .chown = lfs_chown,
  .truncate = lfs_truncate,
  .flush = lfs_flush,
  .release = lfs_release,
  .fsync = lfs_fsync,
  .opendir = lfs_opendir,
  .releasedir = lfs_releasedir,
  .init = lfs_init,
  .access = lfs_access,
  .getxattr = lfs_getxattr,
};

int main(int argc, char *argv[])
{
	struct private_state* lfs_private_data = (struct private_state *)malloc(sizeof(struct private_state));
	assert(NULL != lfs_private_data);
    lfs_private_data->rootdir = realpath(argv[argc-1], NULL);  // 用于形成完整路径

    // 初始化全局变量li
	li = (struct lfs_global_info*)malloc(sizeof(struct lfs_global_info));
    
	// 为缓存当前段分配空间并初始化为0
	li->cur_seg_buf = malloc(SEG_SIZE);
	memset((void*)li->cur_seg_buf, 0, SEG_SIZE);

	// 将当前段中第一个块的inode_num和logical_blk均设置为-1，指明这个块是段摘要块
	struct segsum *ss = (struct segsum*)(li->cur_seg_buf);	
	ss[0].inode_num = -1;
	ss[0].logical_blk = -1;

	li->fih 	   = NULL;
	li->cur_seg_blk    = 1;  // 第0个块用做段摘要块了，段中当前块从1开始
	li->log_head	   = 0;
	li->n_inode	   = 0;
	li->threshold	   = THRESHOLD;
	
	// allocate memory to bitmap and set all values as 1 indicating
	// all free segments initiallly
	for(int i = 0; i< MAX_NUM_SEG; i++)
		li->seg_bitmap[i] = 1;	// 为1表示空闲段

	// initialize all the inode map entries to -1
	for(int i=0;i<MAX_INODES;i++)
	{
		li->ino_map[i].seg_num = -1;
		li->ino_map[i].blk_num = -1;
	}

    // 打开用做文件系统持久存储的文件
	// open是linux硬件设备操作函数，O_RDWR表示以可读可写的方式打开
	li->fd = open("./lfslog", O_RDWR);
	assert(li->fd > 0);

    // 将磁盘内容清0
	int file_disk_size = (SEG_SIZE * MAX_NUM_SEG + BLKSIZE);  // 一个段的字节数*段的数量 + 一个块的字节数
	char *file_disk_buf = malloc(file_disk_size);
	memset((void*)file_disk_buf, 0, file_disk_size);
	pwrite(li->fd, file_disk_buf, file_disk_size, 0);
	free(file_disk_buf);

    // 控制权转给FUSE
    return fuse_main(argc, argv, &lfs_oper, lfs_private_data);
}
