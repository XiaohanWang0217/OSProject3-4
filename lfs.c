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

//#include <threads.h>  // 互斥操作的声明文件

#include <semaphore.h>  // 改用信号量试试

#include "lfs.h"  // 文件系统的数据结构
#include "prettyprint.h"  // 自己写的输出各种结构体的函数
#include "lfsqueue.h"  // 以层序保存多级目录哈希表时用的辅助队列

// 全局变量
struct lfs_global_info *li;
// 全局互斥对象
sem_t lfs_golbal_sem;
// 细粒度互斥对象
sem_t lfs_sem_getattr;
sem_t lfs_sem_opendir;
sem_t lfs_sem_readdir;
sem_t lfs_sem_open;
sem_t lfs_sem_read;
sem_t lfs_sem_link;
sem_t lfs_sem_rename;
sem_t lfs_sem_unlink;
sem_t lfs_sem_write;
sem_t lfs_sem_fsync;
sem_t lfs_sem_chown;
sem_t lfs_sem_chmod;
sem_t lfs_sem_create;
sem_t lfs_sem_mkdir;
sem_t lfs_sem_rmdir;
sem_t lfs_sem_fsyncdir;
sem_t lfs_sem_destroy;

// 由参数中的路径生成完整路径
static void lfs_fullpath(char fpath[PATH_MAX], const char *path)
{
    struct private_state * echofs_private_state = (struct private_state *) fuse_get_context()->private_data;
    strcpy(fpath, echofs_private_state->rootdir);
    strncat(fpath, path, PATH_MAX); // ridiculously long paths will break here
}

// 由逻辑块号计算得到块的类型
int get_blk_type(int32_t logical_blk, int * idx_in_inode, int* idx_in_undirect_blk)
{
	assert(logical_blk >= 0);

	if (logical_blk < MAX_DIRECT_BLKS_INODE)
	{
		*idx_in_inode = logical_blk;
		return DIRECT_DATA_BLK;
	}
	else
	{
		*idx_in_inode = (logical_blk - MAX_DIRECT_BLKS_INODE) / (MAX_BLKS_4_UNDIRECT + 1);  // 间接块也占逻辑块号
		*idx_in_undirect_blk = (logical_blk - MAX_DIRECT_BLKS_INODE) % (MAX_BLKS_4_UNDIRECT + 1) - 1;
		if (*idx_in_undirect_blk < 0)
		{
			return UNDIRECT_BLK;
		}
		else
		{
			return UNDIRECT_DATA_BLK;
		}
	}
}

// 寻找下一个标志为空闲的段，若找不到，则返回-1
int get_next_free_segment()
{
	int j = li->log_head + 1;
    for(int i = 0; i < MAX_NUM_SEG ; i++, j++) 
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
	int count = 0;
	for(int i = 0; i < MAX_NUM_SEG; i++)
	{
		if(li->seg_bitmap[i] == 1)
		{
			count++;
		}
	}
	return count;
}

// 从磁盘中指定位置读出指定数量的字节
void read_from_log(int seg_num, int block_num, char *buf, int size, int blk_offset)
{
    int offset;
    offset = seg_num * SEG_SIZE + block_num * BLKSIZE + SUPERBLKSIZE + blk_offset;
    pread(li->fd, buf, size, offset);
    return;
}

// 返回指定段中dead的块数，ssbuf中为该段摘要块内容
int get_dead_blk_num_in_seg(int segno, char *ssbuf)
{
	int deadblock = 0; 
	char *ibuf = malloc(BLKSIZE);

	// 读入当前段的段摘要块内容到ssbuf中
	read_from_log(segno, 0, ssbuf, BLKSIZE, 0);
	struct lfs_seg_sum *ss = (struct lfs_seg_sum *) ssbuf;

	for(int j = 1; j < MAX_SEG_BLKS; j++)
	{
		// 该段每一个块的摘要
		int ino = ss[j].inode_num;
        int logical_blk = ss[j].logical_blk;

		if(logical_blk == -1) // 该块为索引结点块
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
			// 该块为文件的第logical_blk个数据块/间接块）
			int idx_in_inode, idx_in_undirect_blk;
			int blk_type = get_blk_type(logical_blk, &idx_in_inode, &idx_in_undirect_blk);

			// 读出该块所属文件的索引结点块
			read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, ibuf, BLKSIZE, 0);
			struct lfs_inode *inode = (struct lfs_inode *) ibuf;
			
			// if block address in inode do not match, mark as a deadblock
			if(blk_type == DIRECT_DATA_BLK && ((inode->direct_blks[idx_in_inode].seg_num != segno) || (inode->direct_blks[idx_in_inode].blk_num != j)))
			{
				ss[j].inode_num = -1;  
				ss[j].logical_blk = -1;
				deadblock++;
			}

            if(blk_type == UNDIRECT_BLK && ((inode->undirect_blks[idx_in_inode].seg_num != segno) || (inode->undirect_blks[idx_in_inode].blk_num != j)))
			{
				ss[j].inode_num = -1;  
				ss[j].logical_blk = -1;
				deadblock++;
			}

            if(blk_type == UNDIRECT_DATA_BLK)
			{
				// 读出该块所属间接块
			    read_from_log(inode->undirect_blks[idx_in_inode].seg_num, inode->undirect_blks[idx_in_inode].blk_num, ibuf, BLKSIZE, 0);
				struct lfs_blk_addr*  blk_addr = (struct lfs_blk_addr*) ibuf;
				if (blk_addr[idx_in_undirect_blk].seg_num != segno || blk_addr[idx_in_undirect_blk].blk_num != j)
				{
					ss[j].inode_num = -1;  
					ss[j].logical_blk = -1;
					deadblock++;
				}
			}
		}
	}
	free(ibuf);
	return deadblock;
}

// clean_segs()和copy_segmentdata_to_log()间有递归调用
void clean_segs();

// 将元数据写入磁盘，操作成功返回0，否则返回-1
int copy_metadata_to_log(int fd)
{
	void* temp_super_blk_buf = malloc(SUPERBLKSIZE);

	// 全局变量中的内容入超级块缓冲区
	struct lfs_global_info* temp_li = (struct lfs_global_info* )temp_super_blk_buf;

	temp_li->fih = NULL;
	temp_li->cur_seg_buf = NULL;
	temp_li->cur_seg_blk = 0;
	temp_li->n_inode = li->n_inode;

	temp_li->log_head = 0;
	for(int i=0; i<temp_li->n_inode; i++)
	{
		temp_li->ino_map[i].seg_num = li->ino_map[i].seg_num;
		temp_li->ino_map[i].blk_num = li->ino_map[i].blk_num;
		temp_li->ino_map[i].nlink = li->ino_map[i].nlink;
		temp_li->ino_map[i].dir_ino = li->ino_map[i].dir_ino;
		strcpy(temp_li->ino_map[i].f_name, li->ino_map[i].f_name);
		temp_li->ino_map[i].is_dir = li->ino_map[i].is_dir;
	}

	for(int i=0; i<MAX_NUM_SEG; i++)
	{
		temp_li->seg_bitmap[i] = li->seg_bitmap[i];
	}

    temp_li->fd = 0;

	temp_li->threshold = li->threshold;

	// 按层序将多级目录中哈希表内容置入超级块缓冲区
	int inodenum=0;
	if(li->fih != NULL)
	{
		off_t offset = sizeof(struct lfs_global_info);
		Queue q;
		int result_qop = initQueue(&q);
		if (result_qop<0)
		    return -1;  // 队列初始化失败 

        struct lfs_file_inode_hash *inode_in_hash, *temp;

        // 先迭代根目录下的
		HASH_ITER(hh, li->fih, inode_in_hash, temp)
		{
			memmove(temp_super_blk_buf+offset, (void *)inode_in_hash, sizeof(struct lfs_file_inode_hash));
			offset += sizeof(struct lfs_file_inode_hash);

			inodenum++;

			if (inode_in_hash->is_dir && inode_in_hash->subfih != NULL)
			{
				result_qop = pushQueue(&q, inode_in_hash);
				if (result_qop < 0)
				    return -1;  // 入队失败
			}   
		}

        //struct lfs_file_inode_hash *ptr_fih;
		while(!isEmptyQueue(q))
		{
			struct lfs_file_inode_hash *ptr_fih = popQueue(&q);
			if (ptr_fih == NULL)
			{
			    return -1;  // 出队失败
			}

			// 迭代子目录下的
			HASH_ITER(hh, ptr_fih->subfih, inode_in_hash, temp)
			{
				memmove(temp_super_blk_buf+offset, (void *)inode_in_hash, sizeof(struct lfs_file_inode_hash));
				offset += sizeof(struct lfs_file_inode_hash);

				inodenum++;

				if (inode_in_hash->is_dir && inode_in_hash->subfih != NULL)
				{
					result_qop = pushQueue(&q, inode_in_hash);
					if (result_qop < 0)
						return -1;  // 入队失败
				}
			}
		}
		destroyQueue(&q);
	}

	// 将缓存的超级块内容写入磁盘
	size_t ret = pwrite(fd, temp_super_blk_buf, SUPERBLKSIZE, 0);
	assert(ret == SUPERBLKSIZE);
	return 0;
}

// 将缓存段写入磁盘并调入空闲段
void copy_segmentdata_to_log(int fd, char * buf, size_t count, off_t offset )
{	
	// 将缓冲段的所有内容写入磁盘
	size_t ret1 = pwrite(fd, buf, count, offset);
    // 将元数据写入磁盘，操作成功返回0，否则返回-1
    int ret2 = copy_metadata_to_log(fd);
	assert(ret1 == count && ret2>=0);

	// 更新段标志，表明指定段中已写入内容
	li->seg_bitmap[li->log_head] = 0;
	
	// 空闲段数不足时执行清理
	if(num_of_free_segments() <= MIN_FREE_SEG_NUM)
	{
		li->cur_seg_blk = 1;
		clean_segs();
	}

	// 获取下一个空闲段的段号
	li->log_head = get_next_free_segment();

	// 将缓存段的内容清0
	memset((void*)li->cur_seg_buf, 0, SEG_SIZE);

	// 重置段摘要块标志
	struct lfs_seg_sum *ss = (struct lfs_seg_sum*)(li->cur_seg_buf);
	ss[0].inode_num = -1;
	ss[0].logical_blk = -1;

	li->cur_seg_blk = 1;
	return;
}

// 清理所有的段
void clean_segs()
{
	char *temp_seg_buf = malloc(SEG_SIZE);  // 一个段大小的空间
	char *seg_sum_blk_buf = malloc(BLKSIZE);  // 一个块大小的辅助空间

	// 对所有的段
	for(int i=0; i < MAX_NUM_SEG; i++)
	{
	    if(li->seg_bitmap[i] == 0)  // 若段非空
	    {
			int dead_blk_num = get_dead_blk_num_in_seg(i, seg_sum_blk_buf);  // dead_blk_num为该段中dead的块数，seg_sum_blk_buf中为该段的段摘要块内容

			if(dead_blk_num >= li->threshold)  // 超过阈值，需要清理
			{
				// 待清理段的内容都读到temp_seg_buf中
				read_from_log(i, 0, temp_seg_buf, SEG_SIZE, 0);

				struct lfs_seg_sum *seg_sum_cur = (struct lfs_seg_sum *) li->cur_seg_buf; // 当前缓存段的段摘要，live的块要复制到此段中
				struct lfs_seg_sum *seg_sum_to_clean = (struct lfs_seg_sum *) seg_sum_blk_buf ; // 当前待清理段的段摘要
				
				// 对当前待清理段中的块逐个处理（从1开始，已跳过段摘要块）
				for(int j = 1; j < MAX_SEG_BLKS; j++)
				{
					int ino = seg_sum_to_clean[j].inode_num;
					int logical_blk = seg_sum_to_clean[j].logical_blk;

					if( ino != -1)  // 若该块不是dead的（执行get_dead_blk_num_in_seg的时候把dead块的inode_num设置为-1的）
					{
						if( li->ino_map[ino].seg_num != li->log_head) // 若该块的索引结点不在内存中（该块索引结点所在的段不是当前缓存的段）
						{
							// 把该块的索引结点块读到当前缓存段的当前块
							// 因为移动块后要更改索引结点中对应条目的内容
							read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, BLKSIZE, 0);

							// 更新inode map中此索引结点的条目(nlink、dir_ino、f_name、is_dir值不变)
							li->ino_map[ino].seg_num = li->log_head; 
							li->ino_map[ino].blk_num = li->cur_seg_blk;
						
							// 更新当前缓存段摘要中此块的条目
							seg_sum_cur[li->cur_seg_blk].inode_num = ino;  
							seg_sum_cur[li->cur_seg_blk].logical_blk = -1;  // 此块为索引结点块 
						
						    // 内存中缓存段的当前块是最后一块时，将缓存的段写入磁盘，并加载新的空闲段
							if( li->cur_seg_blk == MAX_SEG_BLKS -1)
							{
								copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
							}
							else
							{
								li->cur_seg_blk++;  // 当前缓存段的块号加1
							}
						}
						
						if( logical_blk == -1) // 当前待处理块是索引结点块，则此块已复制到当前缓存段
						{
							continue;  
						}
 
						// 当前待处理块是数据块或间接块，其索引结点可能是原先就在缓存段中的，也可能是刚读入的
						struct lfs_inode *inode_blk = (struct lfs_inode *) (li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE); 

						// 该块为文件的第logical_blk个数据块/间接块
						int idx_in_inode, idx_in_undirect_blk;
						int blk_type = get_blk_type(logical_blk, &idx_in_inode, &idx_in_undirect_blk);

						if(blk_type == DIRECT_DATA_BLK)
						{
							// 把该块（之前已读入到temp_seg_buf中）内容移动到缓存段的当前块
							memmove(li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, temp_seg_buf + j*BLKSIZE, BLKSIZE); 

							// 更新索引结点中对此块的记录条目
							inode_blk->direct_blks[idx_in_inode].seg_num = li->log_head; 
							inode_blk->direct_blks[idx_in_inode].blk_num = li->cur_seg_blk;
							
							// 更新当前缓存段的段摘要
							seg_sum_cur[li->cur_seg_blk].inode_num = ino; 
							seg_sum_cur[li->cur_seg_blk].logical_blk = logical_blk; 

							// 内存中缓存段的当前块是最后一块（函数内部会判断）时，将缓存的段写入磁盘
							if( li->cur_seg_blk == MAX_SEG_BLKS -1)
							{
								copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE,li->log_head * SEG_SIZE + SUPERBLKSIZE);
							}
							else
							{
								li->cur_seg_blk++;  // 缓存段当前块号加1
							}
						}

						if(blk_type == UNDIRECT_BLK)
						{
							// 把该块（之前已读入到temp_seg_buf中）内容移动到缓存段的当前块
							memmove(li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, temp_seg_buf + j*BLKSIZE, BLKSIZE); 

							// 更新索引结点中对此块的记录条目
							inode_blk->undirect_blks[idx_in_inode].seg_num = li->log_head; 
							inode_blk->undirect_blks[idx_in_inode].blk_num = li->cur_seg_blk;
							
							// 更新当前缓存段的段摘要
							seg_sum_cur[li->cur_seg_blk].inode_num = ino; 
							seg_sum_cur[li->cur_seg_blk].logical_blk = logical_blk; 

							// 内存中缓存段的当前块是最后一块（函数内部会判断）时，将缓存的段写入磁盘
							if( li->cur_seg_blk == MAX_SEG_BLKS -1)
							{
								copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE,li->log_head * SEG_SIZE + SUPERBLKSIZE);
							}
							else
							{
								li->cur_seg_blk++;  // 缓存段当前块号加1
							}
						}

						if(blk_type == UNDIRECT_DATA_BLK)
						{
							// 把该块（之前已读入到temp_seg_buf中）内容移动到缓存段的当前块
							memmove(li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, temp_seg_buf + j*BLKSIZE, BLKSIZE); 

							// 记录此间接块指向的数据块的条目
							int undirect_data_blk_seg_num = li->log_head; 
							int undirect_data_blk_num = li->cur_seg_blk;
							
							// 更新当前缓存段的段摘要
							seg_sum_cur[li->cur_seg_blk].inode_num = ino; 
							seg_sum_cur[li->cur_seg_blk].logical_blk = logical_blk; 

							// 内存中缓存段的当前块是最后一块（函数内部会判断）时，将缓存的段写入磁盘
							if( li->cur_seg_blk == MAX_SEG_BLKS -1)
							{
								copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
							}
							else
							{
								li->cur_seg_blk++;  // 缓存段当前块号加1
							}

							// 该块所属间接块不在当前段中，则将其读入当前段，因为要修改其中条目
							if (inode_blk->undirect_blks[idx_in_inode].seg_num != li->log_head)
							{
								read_from_log(inode_blk->undirect_blks[idx_in_inode].seg_num, inode_blk->undirect_blks[idx_in_inode].blk_num, li->cur_seg_buf + li->cur_seg_blk *BLKSIZE, BLKSIZE, 0);
							    
								// 更新索引结点中该间接块的条目
								inode_blk->undirect_blks[idx_in_inode].seg_num = li->log_head;
								inode_blk->undirect_blks[idx_in_inode].blk_num = li->cur_seg_blk;

								// 更新当前缓存段摘要中此块的条目
								seg_sum_cur[li->cur_seg_blk].inode_num = ino;  
								seg_sum_cur[li->cur_seg_blk].logical_blk = MAX_DIRECT_BLKS_INODE + idx_in_inode * (MAX_BLKS_4_UNDIRECT + 1);
							
								// 内存中缓存段的当前块是最后一块时，将缓存的段写入磁盘，并加载新的空闲段
								if( li->cur_seg_blk == MAX_SEG_BLKS -1)
								{
									copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
								}
								else
								{
									li->cur_seg_blk++;  // 当前缓存段的块号加1
								}
							}

							// 该块所属间接块已在当前段中
							struct lfs_blk_addr * undirect_data_blk = (struct lfs_blk_addr *)(li->cur_seg_buf + inode_blk->undirect_blks[idx_in_inode].blk_num * BLKSIZE);
							undirect_data_blk[idx_in_undirect_blk].seg_num = undirect_data_blk_seg_num;
							undirect_data_blk[idx_in_undirect_blk].blk_num = undirect_data_blk_num;
						}
					}
				}	
				// 已完成清理的段标志为空闲
				li->seg_bitmap[i] = 1;
		    }
		}
	}
	return;
}

// 从路径中提取文件名部分
char* get_filename(const char *path)
{
	int i=0;
	while ( *(path+i) != '\0' ) i++;
	while ( *(path+i) != '/' ) i--;
	i++;
	return path+i;
}

// 在哈希表中查找指定的文件或目录（非根目录）
// 找到则返回指向结点的指针，没找到则返回NULL
struct lfs_file_inode_hash * find_inode_by_path_in_hash(const char*path){
	int pathlen = strlen(path);
    assert(pathlen>0);
	assert(strcmp(path, "/") != 0); // 不是根目录

    // 去除起始的表示根目录的斜杠
    char path1[MAXNAMELEN];
	strncpy(path1, path+1, pathlen-1);
	path1[pathlen-1] = '\0';
	pathlen = strlen(path1);

	struct lfs_file_inode_hash *inode_in_hash, *ptr_fih=li->fih;
	char tempname[MAXNAMELEN];
	while(pathlen > 0){
		char* chsplit = strstr(path1, "/");  //在路径中从前往后找分隔符
		if (NULL == chsplit)
		{
			// path1中整个是名称了
			strncpy(tempname, path1, pathlen); 
			tempname[pathlen] = '\0';
			pathlen = 0;
		}
		else
		{
			// 取出当前层子目录的名称
			strncpy(tempname, path1, chsplit-path1);
			tempname[chsplit-path1] = '\0';
			pathlen = pathlen - strlen(tempname) - 1;  // 去除子目录名及其后的分隔符
			if(pathlen>0)
			{
				strncpy(path1, chsplit+1, pathlen);  // 剩余的路径
				path1[pathlen] = '\0';
			}
		}

		// 在ptr_fih指向的目录下内容哈希表中查找指定名称文件或目录对应的结点，找到就令inode_in_hash指向它
	    HASH_FIND_STR(ptr_fih, tempname, inode_in_hash);
		if (inode_in_hash==NULL || (pathlen>0 && !inode_in_hash->is_dir))
		{
			// 出错，当前层未找到指定名称的文件或目录
			// 或当前名称为目录，但实际存储中标记它为文件
			return NULL;
		}

		if (pathlen == 0){// 找到最底层的了
			return inode_in_hash;
		}
		else{
			ptr_fih = inode_in_hash->subfih; // 进入下一层
		}
	}
	return NULL;  // 这句应该执行不到
}

// 将用做文件系统持久存储的文件的内容清0
void clear_log()
{
	// 初始化全局变量li
	li->fih = NULL;

	// 为缓存当前段分配空间并初始化为0
	li->cur_seg_buf = malloc(SEG_SIZE);
	memset((void*)li->cur_seg_buf, 0, SEG_SIZE);

	// 将当前段中第一个块的inode_num和logical_blk均设置为-1，指明这个块是段摘要块
	struct lfs_seg_sum *seg_sum = (struct lfs_seg_sum*)(li->cur_seg_buf);	
	seg_sum[0].inode_num = -1;
	seg_sum[0].logical_blk = -1;

	li->cur_seg_blk = 1;  // 第0个块用做段摘要块了，段中当前块从1开始
	li->log_head = 0;
	li->n_inode = 0;
	li->threshold = THRESHOLD;
	
	// allocate memory to bitmap and set all values as 1 indicating
	// all free segments initiallly
	for(int i = 0; i< MAX_NUM_SEG; i++)
		li->seg_bitmap[i] = 1;	// 为1表示空闲段

	// initialize all the inode map entries to -1
	for(int i=0;i<MAX_INODES;i++)
	{
		li->ino_map[i].seg_num = -1;
		li->ino_map[i].blk_num = -1;
		li->ino_map[i].nlink = 0;
		li->ino_map[i].dir_ino = -2;
		strcpy(li->ino_map[i].f_name, "");
        li->ino_map[i].is_dir = 0;
	}

    // 将磁盘内容清0
	int file_disk_size = (SEG_SIZE * MAX_NUM_SEG + SUPERBLKSIZE);  // 一个段的字节数*段的数量 + 一个超级块的字节数
	char *file_disk_buf = malloc(file_disk_size);
	memset((void*)file_disk_buf, 0, file_disk_size);
	pwrite(li->fd, file_disk_buf, file_disk_size, 0);
	free(file_disk_buf);
}

// 从磁盘文件的超级块中加载元数据，操作成功返回0，否则返回-1
int load_metadata_from_log(int fd)
{
	// 读入磁盘文件中超级块的内容
    char* temp_super_blk_buf = (char*)malloc(SUPERBLKSIZE);
	pread(li->fd, temp_super_blk_buf, SUPERBLKSIZE, 0);

	// 加载超级块中的元数据
	struct lfs_global_info * temp_li = (struct lfs_global_info *)temp_super_blk_buf;

	if(temp_li->n_inode <= 0)  // 空的
	{
	    return -1;
	}

	// 校验索引结点map中记录是否有效
	for(int i=0; i<temp_li->n_inode; i++)
	{
		if(temp_li->ino_map[i].seg_num < 0 || temp_li->ino_map[i].blk_num < 0 || temp_li->ino_map[i].nlink < 1 || strlen(temp_li->ino_map[i].f_name) <= 0)
		{
			return -1;
		}
		int temp_dir_ino = temp_li->ino_map[i].dir_ino;
		int temp_n_inode = temp_li->n_inode;
		if(temp_dir_ino < -1 || temp_dir_ino>=temp_n_inode || (temp_dir_ino>=0 && temp_li->ino_map[temp_dir_ino].is_dir==0))
		{
			return -1;
		}
	}

	li->fih = NULL;
	li->n_inode = temp_li->n_inode;

	for(int i=0; i<temp_li->n_inode; i++)
	{
		li->ino_map[i].seg_num = temp_li->ino_map[i].seg_num;
		li->ino_map[i].blk_num = temp_li->ino_map[i].blk_num;
		li->ino_map[i].nlink = temp_li->ino_map[i].nlink;
		li->ino_map[i].dir_ino = temp_li->ino_map[i].dir_ino;
		strcpy(li->ino_map[i].f_name, temp_li->ino_map[i].f_name);
		li->ino_map[i].is_dir = temp_li->ino_map[i].is_dir;
	}
	for(int i=temp_li->n_inode; i<MAX_INODES; i++)
	{
		li->ino_map[i].seg_num = -1;
		li->ino_map[i].blk_num = -1;
		li->ino_map[i].nlink = 0;
		li->ino_map[i].dir_ino = -2;
		strcpy(li->ino_map[i].f_name, "");
		li->ino_map[i].is_dir = 0;
	}

    // 准备好层序恢复的辅助队列
	Queue q;
	int result_qop = initQueue(&q);
	if (result_qop<0)
		return -1;  // 队列初始化失败 

	// 恢复哈希表
	int inodecount = 0;
	int32_t current_dir_inode_num = -1;  // 根目录
	off_t offset = sizeof(struct lfs_global_info);
	struct lfs_file_inode_hash *ptr_fih;

	struct lfs_file_inode_hash *inode_in_hash = (struct lfs_file_inode_hash *)(temp_super_blk_buf + offset);
    offset += sizeof(struct lfs_file_inode_hash);
	pretty_print_lfs_file_inode_hash(inode_in_hash);
	while(inodecount < li->n_inode && offset < SUPERBLKSIZE - sizeof(struct lfs_file_inode_hash))
	{
        // 校验
		if(li->ino_map[inode_in_hash->inode_num].dir_ino == current_dir_inode_num)
		{
			// 是当前目录下的内容
			if (current_dir_inode_num == -1)
			{
				// 根目录下
				HASH_ADD_STR(li->fih, f_name, inode_in_hash);
			}
			else
			{
				HASH_ADD_STR(ptr_fih->subfih, f_name, inode_in_hash);
			}

			if(inode_in_hash->subfih != NULL)
			{
				inode_in_hash->subfih = NULL;
				int result_qop = pushQueue(&q, inode_in_hash);
				if (result_qop < 0)
				{			
				    return -1;  // 入队失败
				}
			}
		}
		else 
		{
			ptr_fih = popQueue(&q);
			if (ptr_fih == NULL)
			{
			    return -1;  // 出队失败
			}
			current_dir_inode_num = ptr_fih->inode_num;
			continue;
		}

		inodecount ++;
		if(inodecount == li->n_inode) break;

		inode_in_hash = (struct lfs_file_inode_hash *)(temp_super_blk_buf + offset);
        offset += sizeof(struct lfs_file_inode_hash);
	    pretty_print_lfs_file_inode_hash(inode_in_hash);
	}
	assert(inodecount == li->n_inode);
	destroyQueue(&q);

	// 为缓存当前段分配空间并初始化为0
	li->cur_seg_buf = malloc(SEG_SIZE);
	memset((void*)li->cur_seg_buf, 0, SEG_SIZE);

	// 空闲段数不足时执行清理
	if(num_of_free_segments() <= MIN_FREE_SEG_NUM)
	{
		li->cur_seg_blk = 1;
		clean_segs();
	}

	// 获取下一个空闲段的段号
	li->log_head = get_next_free_segment();

	// 将缓存段的内容清0
	memset((void*)li->cur_seg_buf, 0, SEG_SIZE);

	// 重置段摘要块标志
	struct lfs_seg_sum *ss = (struct lfs_seg_sum*)(li->cur_seg_buf);
	ss[0].inode_num = -1;
	ss[0].logical_blk = -1;

	li->cur_seg_blk = 1;

	for(int i=0; i<MAX_NUM_SEG; i++)
	{
		li->seg_bitmap[i] = temp_li->seg_bitmap[i];
	}

	li->threshold = temp_li->threshold;
	return 0;
}

// 检查当前用户的读权限
int checkReadMod(int owner_num, int group_num, mode_t mode)
{
	struct fuse_context *cxt = fuse_get_context();
	if (((mode & 4) != 0) || ((owner_num == cxt->uid) && ((mode & 64) != 0)) || ((group_num == cxt->gid) && ((mode & 1024) != 0)))
	{
		printf("\nread permission ok\n");
		return 1;
	}
	else
	{
		printf("\nread permission error\n");
		return 0;
	}
}

// 检查当前用户的写权限
int checkWriteMod(int owner_num, int group_num, mode_t mode)
{
	struct fuse_context *cxt = fuse_get_context();
	if (((mode & 2) > 0) || ((owner_num == cxt->uid) && ((mode & 32) != 0)) || ((group_num == cxt->gid) && ((mode & 512) != 0)))
	{
		printf("\nwrite permission ok\n");
		return 1;
	}
	else
	{
		printf("\nwrite permission error\n");
		return 0;
	}
}

// 核心各操作功能代码

/** Initialize filesystem
* The return value will passed in the `private_data` field of `struct fuse_context` to all file operations,
* and as a parameter to the destroy() method. It overrides the initial value provided to fuse_main() / fuse_new().
*/
static void *lfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	printf("\nWXH LFS INIT\nconn=0x%08lx, cfg=0x%08lx\n", (unsigned long)conn, (unsigned long)cfg);
	
	int sem_result = sem_wait(&lfs_golbal_sem);
	assert(sem_result == 0);

	pretty_print_conn(conn);
	pretty_print_config(cfg);

    sem_result = sem_post(&lfs_golbal_sem);
	assert(sem_result == 0);
    return (struct private_state *) fuse_get_context()->private_data;
}

/** Get file attributes. Similar to stat().
* The 'st_dev' and 'st_blksize' fields are ignored.
* The 'st_ino' field is ignored except if the 'use_ino' mount option is given.
* In that case it is passed to userspace, but libfuse and the kernel will still assign a different inode for internal use (called the "nodeid").
*/
// 根据自定义文件系统中指定文件或目录的信息，填充一个文件状态结构类型的数据，提供给FUSE
static int lfs_getattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS GETATTR\npath=\"%s\", statbuf=0x%08lx, fi=0x%08lx\n", fpath, (unsigned long)statbuf, (unsigned long)fi);
	pretty_print_stat(statbuf);
    if (fi)
	{
        pretty_print_fi(fi);
	}

	int pathlen = strlen(path);
    assert(pathlen>0);

	struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);  // 当前时间

	// 要根据我们的文件系统中该文件的信息，填充fuse中的文件状态结构类型数据
	memset(statbuf, 0, sizeof(struct stat));  // 初始化为全0
	if (strcmp(path, "/") == 0)  // 根目录
	{
		int sem_result = sem_wait(&lfs_sem_getattr);
	    assert(sem_result == 0);

		statbuf->st_mode = S_IFDIR | 0755;  // 标志出该路径是一个目录
		statbuf->st_nlink = 2;  // 根目录的硬链接数量为什么是2呢？
		struct fuse_context *cxt = fuse_get_context();
		statbuf->st_uid = cxt->uid;  // 调用进程的用户ID
		statbuf->st_gid = cxt->gid;
		statbuf->st_size = 0;
		statbuf->st_atim = ts;
		statbuf->st_mtim = ts;
		statbuf->st_ctim = ts;

		sem_result = sem_post(&lfs_sem_getattr);
		assert(sem_result == 0);

		return 0;
	}
	else
	{
		int sem_result = sem_wait(&lfs_sem_getattr);
	    assert(sem_result == 0);
		
		struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
		
		sem_result = sem_post(&lfs_sem_getattr);
		assert(sem_result == 0);

		if(NULL != inode_in_hash){
			inode_in_hash->atim = ts; // 更新文件/目录的最后被访问时间
			if(inode_in_hash->is_dir){
				statbuf->st_mode = inode_in_hash->mode;  // 标志出，该文件是一个目录
				statbuf->st_nlink = li->ino_map[inode_in_hash->inode_num].nlink;  // 硬链接数量
				statbuf->st_uid = inode_in_hash->owner_num;
				statbuf->st_gid = inode_in_hash->group_num;
				statbuf->st_size = 0;
				statbuf->st_atim = inode_in_hash->atim;
				statbuf->st_mtim = inode_in_hash->mtim;
				statbuf->st_ctim = inode_in_hash->ctim;
			}
			else{
				statbuf->st_mode = inode_in_hash->mode;  // 标志出，该文件是一个普通文件
				statbuf->st_nlink = li->ino_map[inode_in_hash->inode_num].nlink;  // 硬链接数量
				statbuf->st_uid = inode_in_hash->owner_num;
				statbuf->st_gid = inode_in_hash->group_num;
				statbuf->st_size = inode_in_hash->f_size;  // 文件大小
				statbuf->st_atim = inode_in_hash->atim;
				statbuf->st_mtim = inode_in_hash->mtim;
				statbuf->st_ctim = inode_in_hash->ctim;
			}

			return 0;
		}
		else
		{
			return -ENOENT; // 出错，不存在
		}
	}
}

/** Open directory
Unless the 'default_permissions' mount option is given, this method should check if opendir is permitted for this directory.
Optionally opendir may also return an arbitrary filehandle in the fuse_file_info structure, which will be passed to readdir, releasedir and fsyncdir.
*/
static int lfs_opendir(const char *path, struct fuse_file_info *fi)
{
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS OPENDIR\npath=\"%s\"\tfi=0x%08lx\n", fpath, (unsigned long)fi);
	pretty_print_fi(fi);

	if(strcmp(path, "/") != 0){
		// 在哈希表中查找指定的文件或目录
        
	    int sem_result = sem_wait(&lfs_sem_opendir);
	    assert(sem_result == 0);

		struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
        //struct fuse_context *cxt = fuse_get_context();

		sem_result = sem_post(&lfs_sem_opendir);
		assert(sem_result == 0);

		if (NULL == inode_in_hash)  
		{
			return -ENOENT;  // 没找到
		}
		else if (!inode_in_hash->is_dir)
		{
			return -ENOTDIR;  // 不是目录
		}
		/*
		else if (inode_in_hash->owner_num != cxt->uid && inode_in_hash->group_num != cxt->gid)  // 调用进程的用户ID与目录所有者、所有者所属组不符
		{
			return -EACCES;  // 权限不符
		}
		*/
	}

    return 0;
}

/** Read directory
 * The filesystem may choose between two modes of operation:
 1) The readdir implementation ignores the offset parameter, and passes zero to the filler function's offset.
    The filler function will not return '1' (unless an error happens), so the whole directory is read in a single readdir operation.
 2) The readdir implementation keeps track of the offsets of the directory entries.
    It uses the offset parameter and always passes non-zero offset to the filler function.
	When the buffer is full (or an error happens) the filler function will return '1'. 
*/
static int lfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS READDIR\npath=\"%s\", buf=0x%08lx, filler=0x%08lx, offset=%ld, fi=0x%08lx, flags=%d\n", fpath, (unsigned long)buf, (unsigned long)filler, offset, (unsigned long)fi, flags);
    pretty_print_fi(fi);

	int sem_result = sem_wait(&lfs_sem_readdir);
	assert(sem_result == 0);

	if (strcmp(path, "/")!=0)
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path);  // 在哈希表中查找指定的目录
		if (ptr_fih == NULL)
		{
			sem_result = sem_post(&lfs_sem_readdir);
		    assert(sem_result == 0);
			return -ENOENT;
		}

		if (!ptr_fih->is_dir)
		{
			sem_result = sem_post(&lfs_sem_readdir);
		    assert(sem_result == 0);
			return -ENOTDIR;  // 不是目录
		}

        if(!checkReadMod(ptr_fih->owner_num, ptr_fih->group_num, ptr_fih->mode))
		{
			sem_result = sem_post(&lfs_sem_readdir);
		    assert(sem_result == 0);
			return -EACCES;  // 权限不符
		}

		struct stat *statbuf;
		struct lfs_file_inode_hash *inode_in_hash, *temp;
		HASH_ITER(hh, ptr_fih->subfih, inode_in_hash, temp)
		{
			statbuf = (struct stat*)malloc(sizeof(struct stat));

			statbuf->st_mode = inode_in_hash->mode;
			statbuf->st_nlink = li->ino_map[inode_in_hash->inode_num].nlink;
			statbuf->st_uid = inode_in_hash->owner_num;
			statbuf->st_gid = inode_in_hash->group_num;
			statbuf->st_size = inode_in_hash->f_size;
			statbuf->st_atim = inode_in_hash->atim;
			statbuf->st_mtim = inode_in_hash->mtim;
			statbuf->st_ctim = inode_in_hash->ctim;

			filler(buf, inode_in_hash->f_name, statbuf, 0, FUSE_FILL_DIR_PLUS);
		}

        sem_result = sem_post(&lfs_sem_readdir);
		assert(sem_result == 0);
		return 0;
	}
	else  // 根目录
	{
		//filler(buf, ".", NULL, 0, FUSE_FILL_DIR_PLUS);
		//filler(buf, "..", NULL, 0, FUSE_FILL_DIR_PLUS);
		struct stat *statbuf;
		struct lfs_file_inode_hash *inode_in_hash, *temp;
		HASH_ITER(hh, li->fih, inode_in_hash, temp)
		{
			statbuf = (struct stat*)malloc(sizeof(struct stat));

			statbuf->st_mode = inode_in_hash->mode;
			statbuf->st_nlink = li->ino_map[inode_in_hash->inode_num].nlink;
			statbuf->st_uid = inode_in_hash->owner_num;
			statbuf->st_gid = inode_in_hash->group_num;
			statbuf->st_size = inode_in_hash->f_size;
			statbuf->st_atim = inode_in_hash->atim;
			statbuf->st_mtim = inode_in_hash->mtim;
			statbuf->st_ctim = inode_in_hash->ctim;

			filler(buf, inode_in_hash->f_name, statbuf, 0, FUSE_FILL_DIR_PLUS);
		}

        sem_result = sem_post(&lfs_sem_readdir);
		assert(sem_result == 0);
        return 0;
	}
}

/** Release directory*/
static int lfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS RELEASEDIR\npath=\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);
	pretty_print_fi(fi);

    return 0;
}

/** Open a file
*   Open flags are available in fi->flags. The following rules apply.
*  - Creation (O_CREAT, O_EXCL, O_NOCTTY) flags will be filtered out / handled by the kernel.
*
*  - Access modes (O_RDONLY, O_WRONLY, O_RDWR, O_EXEC, O_SEARCH)
*    should be used by the filesystem to check if the operation is
*    permitted.  If the ``-o default_permissions`` mount option is
*    given, this check is already done by the kernel before calling
*    open() and may thus be omitted by the filesystem.
*
*  - When writeback caching is enabled, the kernel may send
*    read requests even for files opened with O_WRONLY. The
*    filesystem should be prepared to handle this.
*
*  - When writeback caching is disabled, the filesystem is expected
*    to properly handle the O_APPEND flag and ensure that each write
*    is appending to the end of the file.
*
*  - When writeback caching is enabled, the kernel will handle O_APPEND.
*    However, unless all changes to the file come through the kernel
*    this will not work reliably. The filesystem should thus either
*    ignore the O_APPEND flag (and let the kernel handle it), or return
*    an error (indicating that reliably O_APPEND is not available).
*
* Filesystem may store an arbitrary file handle (pointer, index, etc) in fi->fh,
* and use this in other all other file operations (read, write, flush, release, fsync).
*
* Filesystem may also implement stateless file I/O and not store anything in fi->fh. 
*
* There are also some flags (direct_io, keep_cache) which the filesystem may set in fi,
* to change the way the file is opened. See fuse_file_info structure in <fuse_common.h> for more details.
*
* If this request is answered with an error code of ENOSYS and FUSE_CAP_NO_OPEN_SUPPORT is set
* in `fuse_conn_info.capable`, this is treated as success and future calls to open will
* also succeed without being send to the filesystem process. 
*/
static int lfs_open(const char *path, struct fuse_file_info *fi) 
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS OPEN\npath\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);

	// 在哈希表中查找指定的文件
	if (strcmp(path, "/")!=0)
	{
		int sem_result = sem_wait(&lfs_sem_open);
	    assert(sem_result == 0);

		struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
        //struct fuse_context *cxt = fuse_get_context();

		sem_result = sem_post(&lfs_sem_open);
		assert(sem_result == 0);

		if ( inode_in_hash== NULL)
		{
			return -ENOENT;  // 不存在
		}
		else if (inode_in_hash->is_dir)
		{
			return -EISDIR; // 是一个目录
		}
		/*  
		else if (inode_in_hash->owner_num != cxt->uid && inode_in_hash->group_num != cxt->gid)  // 调用进程的用户ID与目录所有者、所有者所属组不符
		{
			return -EACCES;  // 权限不符
		}
		*/
		return 0;
	}

	return -EISDIR;  // 是根目录
}

/** Read data from an open file
 * Read should return exactly the number of bytes requested except on EOF or error,
 * otherwise the rest of the data will be substituted with zeroes.
 * An exception to this is when the 'direct_io' mount option is specified,
 * in which case the return value of the read system call will reflect the return value of this operation.
*/
static int lfs_read(const char *path, char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS READ\npath=\"%s\", buf=0x%08lx, count=%ld, offset=%ld, fi=0x%08lx\n", fpath, (unsigned long)buf, (unsigned long)count, offset, (unsigned long)fi);

	// 在哈希表中查找指定的文件
	if (strcmp(path, "/")==0)  // 根目录
	{
		return -EISDIR;
	}
	
	int sem_result = sem_wait(&lfs_sem_read);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
	if( inode_in_hash == NULL)
	{
		sem_result = sem_post(&lfs_sem_read);
		assert(sem_result == 0);
		return -ENOENT; // 该文件不存在
	}
	if (inode_in_hash->is_dir)  
	{
		sem_result = sem_post(&lfs_sem_read);
		assert(sem_result == 0);
		return -EISDIR;  // 是目录名
	}
	
	printf("aaa\n");
	if(!checkReadMod(inode_in_hash->owner_num, inode_in_hash->group_num, inode_in_hash->mode))
	{
		sem_result = sem_post(&lfs_sem_read);
		assert(sem_result == 0);
		return -EACCES;  // 权限不符
	}
    printf("bbb\n");

	// 找到文件，找到该文件的索引结点块
	uint32_t ino = inode_in_hash->inode_num;
	char *temp_blk_buf = malloc(BLKSIZE);
    struct lfs_inode *inode_blk;
	if(li->ino_map[ino].seg_num != li->log_head)
	{
		// 索引结点不在当前缓存的段中，则单独读出该索引结点块，放在临时缓存块中
		read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, temp_blk_buf, BLKSIZE, 0);
		inode_blk = (struct lfs_inode *) temp_blk_buf;
	}
	else
	{
		// 索引结点在当前缓存的段中，找到该块
		inode_blk = (struct lfs_inode *) (li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE);
	}

    // 起始位置超出了文件总长度，则返回0
	if(offset >= inode_blk->size)
	{
		sem_result = sem_post(&lfs_sem_read);
		assert(sem_result == 0);
		return 0;
	}

    // 起始位置到文件尾的字节数小于准备读的字节数，则以实际能读的字节数为准
    if (inode_blk->size - offset < count)
	{
		count = inode_blk->size - offset;
	}

	memset(buf, 0, count);  // 按待读出的字节数准备好缓冲区
	for(int pos = offset; pos < offset + count; )  // pos是文件中的偏移位置
	{
		int n = BLKSIZE - pos % BLKSIZE < offset + count - pos ? BLKSIZE - pos % BLKSIZE : offset + count - pos;  // 根据这个偏移量，从一个块中可以读出的字节数
		int blk = pos/BLKSIZE;  // 得到当前待读取字节属于文件中的第几个数据块

		if (blk < MAX_DIRECT_BLKS_INODE)  // 直接数据块
		{
			if(inode_blk->direct_blks[blk].seg_num == li->log_head)
			{
				// 如果这个数据块在当前缓存段中，则读入n个字节
				memmove(buf,  li->cur_seg_buf + inode_blk->direct_blks[blk].blk_num * BLKSIZE+ pos % BLKSIZE, n);
			}
			else{
				// 如果这个数据块不在当前缓存段中，则从磁盘读入该块中的n个字节
				read_from_log(inode_blk->direct_blks[blk].seg_num, inode_blk->direct_blks[blk].blk_num, buf, n, pos % BLKSIZE);
			}
		}
		else  // 间接数据块
		{
			int idx_in_inode = (blk - MAX_DIRECT_BLKS_INODE) / MAX_BLKS_4_UNDIRECT;  // 这里的blk值不包含间接块计数的
			int idx_in_undirect_blk = (blk - MAX_DIRECT_BLKS_INODE - idx_in_inode * MAX_BLKS_4_UNDIRECT);

			struct lfs_blk_addr *undirect_blk;
			if(inode_blk->undirect_blks[idx_in_inode].seg_num != li->log_head)
			{
				// 间接块不在当前缓存的段中，则单独读出该间接块，放在临时缓存块中
				read_from_log(inode_blk->undirect_blks[idx_in_inode].seg_num, inode_blk->undirect_blks[idx_in_inode].blk_num, temp_blk_buf, BLKSIZE, 0);
				undirect_blk = (struct lfs_blk_addr *) temp_blk_buf;
			}
			else
			{
				// 间接块在当前缓存的段中，找到该块
				undirect_blk = (struct lfs_blk_addr *) (li->cur_seg_buf + inode_blk->undirect_blks[idx_in_inode].blk_num* BLKSIZE);
			}

			if(undirect_blk[idx_in_undirect_blk].seg_num == li->log_head)
			{
				// 如果这个数据块在当前缓存段中，则读入n个字节
				memmove(buf,  li->cur_seg_buf + undirect_blk[idx_in_undirect_blk].blk_num * BLKSIZE+ pos % BLKSIZE, n);
			}
			else{
				// 如果这个数据块不在当前缓存段中，则从磁盘读入该块中的n个字节
				read_from_log(undirect_blk[idx_in_undirect_blk].seg_num, undirect_blk[idx_in_undirect_blk].blk_num, buf, n, pos % BLKSIZE);
			}
		}

		pos += n;
		buf += n;
	}

	sem_result = sem_post(&lfs_sem_read);
	assert(sem_result == 0);

	return count;
}

/** Possibly flush cached data
* This is not equivalent to fsync().  It's not a request to sync dirty data.
* Flush is called on each close() of a file descriptor,
* as opposed to release which is called on the close of the last file descriptor for a file.
* Under Linux, errors returned by flush() will be passed to userspace as errors from close(),
* so flush() is a good place to write back any cached dirty data.
* However, many applications ignore errors on close(), and on non-Linux systems, 
* close() may succeed even if flush() returns an error.
* For these reasons, filesystems should not assume that errors returned by flush will ever be noticed or even delivered.
* The flush() method may be called more than once for each open().
* This happens if more than one file descriptor refers to an open file handle,
* e.g. due to dup(), dup2() or fork() calls.  It is not possible to determine if a flush is final,
* so each flush should be treated equally.  Multiple write-flush sequences are relatively rare, so this shouldn't be a problem.
* Filesystems shouldn't assume that flush will be called at any particular point.  It may be called more times than expected, or not at all.
*/
static int lfs_flush(const char *path, struct fuse_file_info *fi)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS FLUSH\npath=\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);
    return 0;
}

/** Release an open file
* Release is called when there are no more references to an open file: all file descriptors are closed and all memory mappings are unmapped.
* For every open() call there will be exactly one release() call with the same flags and file handle.
* It is possible to have a file opened more than once, in which case only the last release will mean,
* that no more reads/writes will happen on the file.  The return value of release is ignored.
*/
static int lfs_release(const char *path, struct fuse_file_info *fi)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS RELEASE\npath=\"%s\", fi=0x%08lx\n", fpath, (unsigned long)fi);
    return 0;
}

/** Create a hard link to a file */
static int lfs_link(const char *path, const char *newpath)
{
	// 自己写
    // 硬链接的意思是一个档案可以有多个名称
    char fpath[PATH_MAX];
	char fnewpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	lfs_fullpath(fnewpath, newpath);
    printf("\nWXH LFS LINK\npath=\"%s\", newpath=\"%s\"\n", fpath, fnewpath);

	// 在哈希表中查找指定的文件
	if (strcmp(path, "/")==0)
	{	
        return -EISDIR;  // 源文件指定为根目录的错
	}

	int sem_result = sem_wait(&lfs_sem_link);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
	if( inode_in_hash == NULL)
	{	
		sem_result = sem_post(&lfs_sem_link);
		assert(sem_result == 0);
		return -ENOENT;  // 源文件不存在
	}
	if (inode_in_hash->is_dir)
	{
		sem_result = sem_post(&lfs_sem_link);
		assert(sem_result == 0);
		return -EISDIR;  // 是个目录
	}

	struct lfs_file_inode_hash *new_inode_in_hash = (struct lfs_file_inode_hash*)malloc(sizeof(struct lfs_file_inode_hash));
	new_inode_in_hash->f_size = inode_in_hash->f_size;
	new_inode_in_hash->inode_num = inode_in_hash->inode_num;
	strcpy(new_inode_in_hash->f_name, get_filename(newpath));  // 新的文件名
	struct fuse_context *cxt = fuse_get_context();
	new_inode_in_hash->owner_num = cxt->uid;  // 调用进程的用户ID
	new_inode_in_hash->group_num = cxt->gid;
	new_inode_in_hash->mode = S_IFREG | 0755;  // 普通文件
	new_inode_in_hash->is_dir = 0;
	new_inode_in_hash->subfih=NULL;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);  // 当前时间
	new_inode_in_hash->atim = ts;
	new_inode_in_hash->mtim = ts;
	new_inode_in_hash->ctim = ts;

	// 查找新的硬链接拟在的路径
	char path1[MAXNAMELEN];
	char * lastsplit = strrchr(newpath, '/');  // 从右向右找目录分隔符
	strncpy(path1, newpath, lastsplit-newpath+1);
	path1[lastsplit-path+1] = '\0';

	if (strcmp(path1, "/")==0)
	{
		HASH_ADD_STR(li->fih, f_name, new_inode_in_hash);  // 根目录下
	}
	else
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path1);
		if (ptr_fih == NULL)
		{
			return -ENOENT;  // 硬链接拟在的路径不存在
		}
		HASH_ADD_STR(ptr_fih->subfih, f_name, new_inode_in_hash);
	}

	// 增加索引结点信息表中相应记录的硬链接数量
	li->ino_map[new_inode_in_hash->inode_num].nlink = li->ino_map[new_inode_in_hash->inode_num].nlink + 1;

    sem_result = sem_post(&lfs_sem_link);
	assert(sem_result == 0);
	return 0;
}

/** Rename a file
 * *flags* may be `RENAME_EXCHANGE` or `RENAME_NOREPLACE`.
 * If RENAME_NOREPLACE is specified, the filesystem must not overwrite *newname* if it exists and return an error instead.
 * If `RENAME_EXCHANGE` is specified, the filesystem must atomically exchange the two files, i.e. both must exist and neither may be deleted.
*/
static int lfs_rename(const char *path, const char *newpath, unsigned int flags)
{
	// 自己写的
    char fpath[PATH_MAX];
	char fnewpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	lfs_fullpath(fnewpath, newpath);
	printf("\nWXH LFS RENAME\nfpath=\"%s\", newpath=\"%s\", flags=%d\n", fpath, fnewpath, flags);
	
	// 例程里查到的
	/* When we have renameat2() in libc, then we can implement flags */
	if (flags)
		return -EINVAL;

	// 在哈希表中查找源文件
	if (strcmp(path, "/")==0)
	{
		return -EISDIR;  // 源文件指定为根目录了
	}
	
	int sem_result = sem_wait(&lfs_sem_rename);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);

	if(inode_in_hash == NULL)
	{
		sem_result = sem_post(&lfs_sem_rename);
		assert(sem_result == 0);
		return -ENOENT;  // 源文件不存在
	}

	if(inode_in_hash->is_dir)
	{
		sem_result = sem_post(&lfs_sem_rename);
		assert(sem_result == 0);
		return -EISDIR;  // 源文件是一个目录
	}

	// 查找新文件名是否存在，若存在则报错，否则生成新文件名，删除原文件名
	struct lfs_file_inode_hash *new_inode_in_hash = find_inode_by_path_in_hash(newpath);
	if (new_inode_in_hash != NULL)
	{
		sem_result = sem_post(&lfs_sem_rename);
		assert(sem_result == 0);
		return -EEXIST;  // 新文件名已存在
	}

	new_inode_in_hash = (struct lfs_file_inode_hash*)malloc(sizeof(struct lfs_file_inode_hash));
	strcpy(new_inode_in_hash->f_name, get_filename(newpath));  // 新的名称
	new_inode_in_hash->f_size = inode_in_hash->f_size;
	new_inode_in_hash->inode_num = inode_in_hash->inode_num;
	new_inode_in_hash->owner_num = inode_in_hash->owner_num;
	new_inode_in_hash->group_num = inode_in_hash->group_num;
	new_inode_in_hash->mode = inode_in_hash->mode;
	new_inode_in_hash->is_dir = inode_in_hash->is_dir;
	new_inode_in_hash->subfih = inode_in_hash->subfih;
	new_inode_in_hash->atim = inode_in_hash->atim;
	new_inode_in_hash->mtim = inode_in_hash->mtim;
	new_inode_in_hash->ctim = inode_in_hash->ctim;

	// 查找新文件拟在的路径
	char path1[MAXNAMELEN];
	char * lastsplit = strrchr(newpath, '/');
	strncpy(path1, newpath, lastsplit-newpath+1);
	path1[lastsplit-newpath+1] = '\0';

	if (strcmp(path1, "/")==0)
	{
		HASH_ADD_STR(li->fih, f_name, new_inode_in_hash);
	}
	else
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path1);
		if (ptr_fih == NULL)
		{
			sem_result = sem_post(&lfs_sem_rename);
		    assert(sem_result == 0);
			return -ENOENT;  // 新文件拟在路径不存在
		}
		HASH_ADD_STR(ptr_fih->subfih, f_name, new_inode_in_hash);
	}

	// 查找原文件所在路径
	lastsplit = strrchr(path, '/');
	strncpy(path1, path, lastsplit-path+1);
	path1[lastsplit-path+1] = '\0';
	if (strcmp(path1, "/")==0)
	{
		HASH_DEL(li->fih, inode_in_hash);
	}
	else
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path1);
		if (ptr_fih == NULL)
		{
			sem_result = sem_post(&lfs_sem_rename);
		    assert(sem_result == 0);
			return -ENOENT;  // 原文件拟在路径不存在
		}
		HASH_DEL(ptr_fih->subfih, inode_in_hash);
	}

	sem_result = sem_post(&lfs_sem_rename);
	assert(sem_result == 0);
	return 0;
}

/**Check file access permissions
* This will be called for the access() system call.
* If the 'default_permissions' mount option is given, this method is not called. 
* This method is not called under Linux kernel versions 2.4.x
*/
static int lfs_access(const char *path, int mask)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS ACCESS\npath=\"%s\", mask=0%o\n", fpath, mask);
    return 0;
}

/**Create a file node
* This is called for creation of all non-directory, non-symlink nodes.
* If the filesystem defines a create() method, then for regular files that will be called instead.
*/
static int lfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS MKNOD\npath=\"%s\", mode=0%3o\tdev=%ld\n", fpath, mode, dev);
    return 0;
}

/** Remove a file */
static int lfs_unlink(const char *path)
{
	char fpath[PATH_MAX];
	lfs_fullpath(fpath, path);
	printf("\nWXH LFS UNLINK\npath=\"%s\"\n", fpath);

	// 在哈希表中查找待删除的文件
	if (strcmp(path, "/")==0)
	{
		return -EISDIR;  // 根目录
	}

	int sem_result = sem_wait(&lfs_sem_unlink);
	assert(sem_result == 0);
	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);

	if(inode_in_hash == NULL)
	{
		sem_result = sem_post(&lfs_sem_unlink);
		assert(sem_result == 0);
		return -ENOENT;  // 文件不存在
	}

	if (inode_in_hash->is_dir)
	{
		sem_result = sem_post(&lfs_sem_unlink);
		assert(sem_result == 0);
		return -EISDIR;  // 是目录，不是文件
	}

	if(!checkWriteMod(inode_in_hash->owner_num, inode_in_hash->group_num, inode_in_hash->mode))
	{
		sem_result = sem_post(&lfs_sem_unlink);
		assert(sem_result == 0);
		return -EACCES;  // 权限不符
	}

	// 更新索引结点表中对应条目，若硬链接数大于1则减少一个，没有其他硬链接则真正删除文件
	int ino = inode_in_hash->inode_num;
	if(li->ino_map[ino].nlink > 1)
	{
		li->ino_map[ino].nlink = li->ino_map[ino].nlink - 1;
	}
	else
	{
		li->ino_map[ino].seg_num = -1;
		li->ino_map[ino].blk_num = -1;
		li->ino_map[ino].nlink = 0;
		li->ino_map[ino].dir_ino = -2;
		strcpy(li->ino_map[ino].f_name, "");
		li->ino_map[ino].is_dir = 0;
	}

	// 从哈希表中删除该文件结点
	// 找到该文件所在的路径
	char path1[MAXNAMELEN];
	char * lastsplit = strrchr(path, '/');
	strncpy(path1, path, lastsplit-path+1);
	path1[lastsplit-path+1] = '\0';
	
	// ptr_fih指向路径入口
	if (strcmp(path1, "/")==0)
	{
		HASH_DEL(li->fih, inode_in_hash);
	}
	else
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path1);
		if(ptr_fih==NULL)
		{
			sem_result = sem_post(&lfs_sem_unlink);
		    assert(sem_result == 0);
			return -ENOENT;  // 上级目录未找到
		}
		else
		{
			HASH_DEL(ptr_fih->subfih, inode_in_hash);
		}
	}

	sem_result = sem_post(&lfs_sem_unlink);
	assert(sem_result == 0);
	return 0;
}

/** Write data to an open file
 * Write should return exactly the number of bytes requested except on error.
 * An exception to this is when the 'direct_io' mount option is specified (see read operation). 
 * Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is expected to reset the setuid and setgid bits.
 */
static int lfs_write(const char *path, const char *buf, size_t count, off_t offset, struct fuse_file_info *fi)
{
	char fpath[PATH_MAX];
	lfs_fullpath(fpath, path);
	printf("\nWXH LFS WRITE\npath=\"%s\", buf=0x%08lx, count=%ld, offset=%ld, fi=0x%08lx\n", fpath, (unsigned long)buf, count, offset, (unsigned long)fi);

	// 在哈希表中查找指定的文件
	if (strcmp(path, "/")==0)
	{
		return -EISDIR;  // 根目录
	}

	int sem_result = sem_wait(&lfs_sem_write);
	assert(sem_result == 0);
	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);

	if(inode_in_hash == NULL)
	{
		sem_result = sem_post(&lfs_sem_write);
		assert(sem_result == 0);
		return -ENOENT;  // 待写入的文件不存在
	}

	if(inode_in_hash->is_dir)
	{
		sem_result = sem_post(&lfs_sem_write);
		assert(sem_result == 0);
		return -EISDIR;  // 是目录，不是文件
	}

	if(!checkWriteMod(inode_in_hash->owner_num, inode_in_hash->group_num, inode_in_hash->mode))
	{
		sem_result = sem_post(&lfs_sem_write);
		assert(sem_result == 0);
		return -EACCES;  // 权限不符
	}

	// 找到待写入文件的索引结点
	uint32_t ino = inode_in_hash->inode_num;

	// 将待写入文件的索引结点块的内容读到缓冲区
	char *temp_blk_buf = malloc(BLKSIZE);
	if(li->ino_map[ino].seg_num != li->log_head) 
	{
		read_from_log(li->ino_map[ino].seg_num, li->ino_map[ino].blk_num, temp_blk_buf, BLKSIZE, 0);
	}
	else
	{
		memmove(temp_blk_buf, li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE, BLKSIZE);
	}
	struct lfs_inode *inode_blk = (struct lfs_inode *) temp_blk_buf;

	// 如果这些字节写入文件，将超出最多允许的数据块数量
	if ( offset + count >= ( MAX_DIRECT_BLKS_INODE + MAX_UNDIRECT_BLKS_INODE * MAX_BLKS_4_UNDIRECT ) * 1024 )
	{
		sem_result = sem_post(&lfs_sem_write);
		assert(sem_result == 0);
		return -EFBIG;  // 文件太大
	}

	// 完成buf中内容的写入
	struct lfs_seg_sum *seg_sum = (struct lfs_seg_sum *) li->cur_seg_buf;  // 指向当前缓冲段的段摘要块
	for(int pos = offset; pos < offset + count;)
	{
		// 能写入当前块的字节数，可能写满一个块，也可能只写若干字节
		uint32_t n = BLKSIZE - pos % BLKSIZE < offset + count - pos ? BLKSIZE - pos % BLKSIZE : offset + count - pos;

		// 更新文件长度信息
		if( pos + n > inode_blk->size)
		{
			// 从这个地方看，这个写操作应该是改写而非插入，即会覆盖掉原来的内容
			inode_blk->size = pos + n; 	// update file size accordingly. 
		}

		int blk = pos/BLKSIZE; // 文件中的第几个数据块
		if (blk < MAX_DIRECT_BLKS_INODE)  // 直接块
		{
			// 这个逻辑块若尚未存在，则得到-1；若已存在，则得到此块所在段号
			int segno = inode_blk->direct_blks[blk].seg_num;
		
			if(segno != -1)  // 逻辑块已存在
			{	
				if( pos % BLKSIZE != 0 )  // pos为0的话也就相当于整块写入了
				{
					if( segno == li->log_head) // 逻辑块已经在内存了，完成buf中内容写入到块
					{				
						memmove( li->cur_seg_buf + inode_blk->direct_blks[blk].blk_num * BLKSIZE, buf, n); 
						pos += n; // 更新写入的起始点（相对于文件中）
						buf += n;
						continue;  // 跳过本次循环后续操作，进入下一次循环
					}
					else
					{
						// 直接从磁盘读入整块的内容到缓存段的当前块
						read_from_log(inode_blk->direct_blks[blk].seg_num, inode_blk->direct_blks[blk].blk_num, li->cur_seg_buf + li->cur_seg_blk*BLKSIZE, BLKSIZE, 0); 
					}
				}
			}
			
			// 在索引结点中更新此逻辑块的信息
			inode_blk->direct_blks[blk].seg_num  = li->log_head;
			inode_blk->direct_blks[blk].blk_num = li->cur_seg_blk; 

			// 将buf中内容写入此块
			memmove( li->cur_seg_buf + (li->cur_seg_blk ) * BLKSIZE, buf, n); 

			// 更新段摘要块中内容
			seg_sum[li->cur_seg_blk].inode_num = ino;
			seg_sum[li->cur_seg_blk].logical_blk = blk; 

			// 如果此块是当前缓存段中的最后一块（函数内会判断），则写入磁盘
			if( li->cur_seg_blk == MAX_SEG_BLKS -1)
			{
				copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
			}
			else
			{
				li->cur_seg_blk++;
			}
		}
		else
		{
			int idx_in_inode = (blk - MAX_DIRECT_BLKS_INODE) / MAX_BLKS_4_UNDIRECT;
			int idx_in_undirect_blk = (blk - MAX_DIRECT_BLKS_INODE) % MAX_BLKS_4_UNDIRECT;

			// 初始化间接块缓存区
			char *temp_undirect_blk_buf = malloc(BLKSIZE);
			struct lfs_blk_addr * blk_addr = (struct lfs_blk_addr *) temp_undirect_blk_buf;
			for(int i=0; i < MAX_BLKS_4_UNDIRECT; i++)
			{
				blk_addr[i].seg_num = -1;
				blk_addr[i].blk_num = -1;
			}

			// 若间接块已存在，则将间接块的内容读到缓冲区
			int undirect_blk_segno = inode_blk->undirect_blks[idx_in_inode].seg_num;
			if(undirect_blk_segno!= -1)
			{
				if (undirect_blk_segno != li->log_head) 
				{
					read_from_log(undirect_blk_segno, inode_blk->undirect_blks[idx_in_inode].blk_num, temp_undirect_blk_buf, BLKSIZE, 0);
				}
				else
				{
					memmove(temp_undirect_blk_buf, li->cur_seg_buf + inode_blk->undirect_blks[idx_in_inode].blk_num * BLKSIZE, BLKSIZE);
				}
			}

			// 这个数据块若尚未存在，则得到-1；若已存在，则得到此块所在段号
			int data_blk_segno = blk_addr[idx_in_undirect_blk].seg_num;
			if(data_blk_segno != -1)  // 数据块已存在
			{	
				if( pos % BLKSIZE != 0 )  // pos为0的话也就相当于整块写入了
				{
					if( data_blk_segno == li->log_head) // 数据块已经在内存了，完成buf中内容写入到块
					{				
						memmove( li->cur_seg_buf + blk_addr[idx_in_undirect_blk].blk_num * BLKSIZE, buf, n); 
						pos += n; // 更新写入的起始点（相对于文件中）
						buf += n;
						continue;  // 跳过本次循环后续操作，进入下一次循环
					}
					else
					{
						// 直接从磁盘读入整块的内容到缓存段的当前块
						read_from_log(blk_addr[idx_in_undirect_blk].seg_num, blk_addr[idx_in_undirect_blk].blk_num, li->cur_seg_buf + li->cur_seg_blk*BLKSIZE, BLKSIZE, 0); 
					}
				}
			}

			// 将buf中内容写入缓存段的当前块
			memmove(li->cur_seg_buf + (li->cur_seg_blk ) * BLKSIZE, buf, n); 

			// 在间接块缓存中更新此数据块的信息
			blk_addr[idx_in_undirect_blk].seg_num = li->log_head;
			blk_addr[idx_in_undirect_blk].blk_num = li->cur_seg_blk; 

			// 更新段摘要块中内容
			seg_sum[li->cur_seg_blk].inode_num = ino;
			seg_sum[li->cur_seg_blk].logical_blk = MAX_DIRECT_BLKS_INODE + idx_in_inode * (MAX_BLKS_4_UNDIRECT+1) + idx_in_undirect_blk;

			// 如果此块是当前缓存段中的最后一块，则写入磁盘
			if( li->cur_seg_blk == MAX_SEG_BLKS -1)
			{
				copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
			}
			else
			{
				li->cur_seg_blk++;
			}
			
			// 用缓存的间接块内容更新此间接块
			if (undirect_blk_segno == li->log_head)
			{
				memmove(li->cur_seg_buf + inode_blk->undirect_blks[idx_in_inode].blk_num * BLKSIZE, temp_undirect_blk_buf, BLKSIZE);
			}
			else
			{
				memmove(li->cur_seg_buf + li->cur_seg_blk * BLKSIZE, temp_undirect_blk_buf, BLKSIZE);

				// 更新段摘要块中内容
				seg_sum[li->cur_seg_blk].inode_num = ino;
				seg_sum[li->cur_seg_blk].logical_blk = MAX_DIRECT_BLKS_INODE + idx_in_inode * (MAX_BLKS_4_UNDIRECT+1);

				// 更新索引结点缓存中的相应条目
				inode_blk->undirect_blks[idx_in_inode].seg_num = li->log_head;
				inode_blk->undirect_blks[idx_in_inode].blk_num = li->cur_seg_blk; 

				// 如果此块是当前缓存段中的最后一块，则写入磁盘
				if( li->cur_seg_blk == MAX_SEG_BLKS -1)
				{
					copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
				}
				else
				{
					li->cur_seg_blk++;
				}
			}
		}

		pos += n; // update pos.
		buf += n;
	}

	// 用缓存的索引结点内容更新此文件的索引结点块
	if(li->ino_map[ino].seg_num == li->log_head)
	{
		// 该索引结点块已在当前缓存段中
		memmove(li->cur_seg_buf + li->ino_map[ino].blk_num * BLKSIZE, temp_blk_buf, BLKSIZE);
	}
	else
	{
		// 将ibuf内容复制到缓存段的当前块中
		memmove(li->cur_seg_buf + li->cur_seg_blk * BLKSIZE, temp_blk_buf, BLKSIZE);

		// 更新inode map中的对应条目，nlink、dir_ino、f_name、is_dir的值不变
		li->ino_map[ino].seg_num = li->log_head; 
		li->ino_map[ino].blk_num = li->cur_seg_blk;

		// 更新段摘要块
		seg_sum[li->cur_seg_blk].inode_num = ino;
		seg_sum[li->cur_seg_blk].logical_blk = -1;

		// 如果此块是当前缓存段中的最后一块（函数内会判断），则写入磁盘
		if( li->cur_seg_blk == MAX_SEG_BLKS -1)
		{
			copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
		}
		else
		{
			li->cur_seg_blk ++;  // 缓存段当前块加1
		}
	}
	
	// 更新哈希表中对应文件的长度信息;
	if(inode_in_hash->f_size != inode_blk->size) 
	{
		inode_in_hash->f_size = inode_blk->size;
	}
	
	sem_result = sem_post(&lfs_sem_write);
	assert(sem_result == 0);
	return count;
}

/** Change the size of a file
* `fi` will always be NULL if the file is not currenlty open, but may also be NULL if the file is open.
* Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is expected to reset the setuid and setgid bits.
*/
static int lfs_truncate(const char *path, off_t newsize, struct fuse_file_info *fi)
{
	// TODO
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH TRUNCATE\npath=\"%s\", newsize=%ld, fi=0x%08lx\n", fpath, newsize, (unsigned long)fi);
    return 0;
}

/** Synchronize file contents
* If the datasync parameter is non-zero, then only the user data should be flushed, not the meta data.
*/
static int lfs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	// 自己写
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH FSYNC\npath=\"%s\", datasync=%d, fi=0x%08lx\n", fpath, datasync, (unsigned long)fi);

	int sem_result = sem_wait(&lfs_sem_fsync);
	assert(sem_result == 0);

	// 将缓冲段的所有内容写入磁盘
	size_t ret1 = pwrite(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
    // 将元数据写入磁盘，操作成功返回0，否则返回-1
    int ret2 = 0;
	if (datasync==0){
		ret2 = copy_metadata_to_log(li->fd);
	}

	sem_result = sem_post(&lfs_sem_fsync);
	assert(sem_result == 0);

	if(ret1 != SEG_SIZE || ret2<0)
	{
		return -EIO;
	}
	else
	{
		return 0;
	}
}

/** Change the owner and group of a file
* `fi` will always be NULL if the file is not currenlty open, but may also be NULL if the file is open.
* Unless FUSE_CAP_HANDLE_KILLPRIV is disabled, this method is expected to reset the setuid and setgid bits.
*/
static int lfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	// 自己写的
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS CHOWN\npath=\"%s\", uid=%d, gid=%d, fi=0x%08lx\n", fpath, uid, gid, (unsigned long)fi);

	// 在哈希表中查找指定的文件
	if (strcmp(path, "/")==0)
	{
		return -EISDIR;  // 不改变根目录的所有者
	}

	int sem_result = sem_wait(&lfs_sem_chown);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
	if (inode_in_hash==NULL)
	{
		sem_result = sem_post(&lfs_sem_chown);
		assert(sem_result == 0);
		return -ENOENT; // 没找到
	}
	inode_in_hash->owner_num = uid;
	inode_in_hash->group_num = gid;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);  // 当前时间
	inode_in_hash->ctim = ts;

	sem_result = sem_post(&lfs_sem_chown);
	assert(sem_result == 0);
	return 0;
}

/** Change the permission bits of a file
* `fi` will always be NULL if the file is not currenlty open, but may also be NULL if the file is open.
*/
static int lfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    // 自己写的
    char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH LFS CHMOD\npath=\"%s\", mode=0%03o, fi=0x%08lx\n", fpath, mode, (unsigned long)fi);
    if (fi)
	{
        pretty_print_fi(fi);
	}

	// 在哈希表中查找指定的文件或目录
	if (strcmp(path, "/")==0)
	{
		return -EISDIR;  // 根目录
	}

	int sem_result = sem_wait(&lfs_sem_chmod);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
	if (inode_in_hash==NULL)
	{
		sem_result = sem_post(&lfs_sem_chmod);
		assert(sem_result == 0);
		return -ENOENT;
	}

	if (inode_in_hash->is_dir)
	{
		inode_in_hash->mode = S_IFDIR | mode;
	}
	else
	{
		inode_in_hash->mode = S_IFREG | mode;
	}

	sem_result = sem_post(&lfs_sem_chmod);
	assert(sem_result == 0);
	return 0; 
}

/** Create and open a file
* If the file does not exist, first create it with the specified mode, and then open it.
*/
static int lfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS CREATE\npath=\"%s\", mode=0%03o, fi=0x%08lx\n", fpath, mode, (unsigned long)fi);
	
	// 在哈希表中查找指定的文件
	if (strcmp(path, "/")==0)
	{
		return -EISDIR;  // 根目录
	}

	int sem_result = sem_wait(&lfs_sem_create);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);

	if(inode_in_hash != NULL)
	{
		if (inode_in_hash->is_dir)  // 同名的目录
		{
		    sem_result = sem_post(&lfs_sem_create);
		    assert(sem_result == 0);
			return -EISDIR;
		}
		else  // 已有同名文件存在
		{
			sem_result = sem_post(&lfs_sem_create);
		    assert(sem_result == 0);
			return -EEXIST;
		}
	}

	// 在当前段中生成新文件的inode块并初始化
	struct lfs_inode *inode_blk = (struct lfs_inode *)(li->cur_seg_buf + (li->cur_seg_blk * BLKSIZE));  // 在当前段的当前块新建一个索引结点
	inode_blk->ino = li->n_inode++;
	inode_blk->size = 0;
	for(int j = 0; j <= MAX_DIRECT_BLKS_INODE; j++)
	{
		inode_blk->direct_blks[j].seg_num = -1;
		inode_blk->direct_blks[j].blk_num = -1;
	}
	for(int j = 0; j <= MAX_UNDIRECT_BLKS_INODE; j++)
	{
		inode_blk->undirect_blks[j].seg_num = -1;
		inode_blk->undirect_blks[j].blk_num = -1;
	}

	// add the newly created inode to for given file into the hash table
	inode_in_hash = (struct lfs_file_inode_hash*)malloc(sizeof(struct lfs_file_inode_hash));
	strcpy(inode_in_hash->f_name, get_filename(path));
	inode_in_hash->inode_num = inode_blk->ino;
	inode_in_hash->f_size = inode_blk->size;
	struct fuse_context *cxt = fuse_get_context();
	inode_in_hash->owner_num = cxt->uid;  // 调用进程的用户ID
	inode_in_hash->group_num = cxt->gid;
	inode_in_hash->mode = S_IFREG | 0755;  // 普通文件
	inode_in_hash->is_dir = 0;
	inode_in_hash->subfih=NULL;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);  // 当前时间
	inode_in_hash->atim = ts;
	inode_in_hash->mtim = ts;
	inode_in_hash->ctim = ts;

	// 找到该文件拟在的路径
	char path1[MAXNAMELEN];
	char * lastsplit = strrchr(path, '/');
	strncpy(path1, path, lastsplit-path+1);
	path1[lastsplit-path+1] = '\0';
	int32_t dir_ino = -2;  // 该文件所在目录的索引结点号
	if (strcmp(path1, "/")==0)
	{
		HASH_ADD_STR(li->fih, f_name, inode_in_hash);
		dir_ino = -1;
	}
	else
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path1);
		if ( ptr_fih==NULL || !ptr_fih->is_dir )
		{
			li->n_inode--;
			sem_result = sem_post(&lfs_sem_create);
		    assert(sem_result == 0);
			return -ENOENT;
		}
		else
		{
			HASH_ADD_STR(ptr_fih->subfih, f_name, inode_in_hash);
			dir_ino = ptr_fih->inode_num; 
		}
	}

	// 记录该索引结点的段号和块号
	li->ino_map[inode_blk->ino].seg_num = li->log_head;
	li->ino_map[inode_blk->ino].blk_num = li->cur_seg_blk; 
	li->ino_map[inode_blk->ino].nlink = 1;
	li->ino_map[inode_blk->ino].dir_ino = dir_ino;
	strcpy(li->ino_map[inode_blk->ino].f_name, inode_in_hash->f_name);
	li->ino_map[inode_blk->ino].is_dir = 0;
	
	// 更新段摘要中内容
	struct lfs_seg_sum *seg_sum = (struct lfs_seg_sum *)(li->cur_seg_buf);
	seg_sum[li->cur_seg_blk].inode_num = inode_blk->ino;
	seg_sum[li->cur_seg_blk].logical_blk = -1;

	// if the in-memory segment is full, write the data to disk
	if( li->cur_seg_blk == MAX_SEG_BLKS -1)
	{
		copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE); 
	}
	else
	{
		li->cur_seg_blk++;  // 当前段的当前块号加1
	}

	sem_result = sem_post(&lfs_sem_create);
	assert(sem_result == 0);
	return 0;
}

/** Get extended attributes */
static int lfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS GETXATTR\npath=\"%s\", name=%s, value=%s, size=%ld\n", fpath, name, value, size);
	return 0;
}

/** Read the target of a symbolic link
* The buffer should be filled with a null terminated string.
* The buffer size argument includes the space for the terminating null character.
* If the linkname is too long to fit in the buffer, it should be truncated.
* The return value should be 0 for success.
*/
static int lfs_readlink(const char *path, char *buf, size_t size)
{
	// TODO
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS READLINK\npath\"%s\", buf=%s, size=%ld\n", fpath, buf, size);
	return 0;
}

/** Create a directory
 * Note that the mode argument may not have the type specification bits set, i.e. S_ISDIR(mode) can be false.
 * To obtain the correct directory type bits use  mode|S_IFDIR
*/
static int lfs_mkdir(const char *path, mode_t mode)
{
	// 自己写的
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS MKDIR\npath=\"%s\", mode=0%3o\n", fpath, mode);

    // 在哈希表中查找指定的目录
	if (strcmp(path, "/")==0)
	{
		return 0;  // 根目录
	}
	
	int sem_result = sem_wait(&lfs_sem_mkdir);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
	if(inode_in_hash != NULL)
	{
		sem_result = sem_post(&lfs_sem_mkdir);
		assert(sem_result == 0);
		return 0;  // 指定名称的目录已存在
	}

	// 在当前段中生成新目录的inode块并初始化
	struct lfs_inode *inode_blk = (struct lfs_inode *)(li->cur_seg_buf + (li->cur_seg_blk * BLKSIZE));  // 在当前段的当前块新建一个索引结点
	inode_blk->ino = li->n_inode++;
	inode_blk->size = 0;
	for(int j = 0; j <= MAX_DIRECT_BLKS_INODE; j++)
	{
		inode_blk->direct_blks[j].seg_num = -1;
		inode_blk->direct_blks[j].blk_num = -1;
	}
	for(int j = 0; j <= MAX_UNDIRECT_BLKS_INODE; j++)
	{
		inode_blk->undirect_blks[j].seg_num = -1;
		inode_blk->undirect_blks[j].blk_num = -1;
	}

	// add the newly created inode to for given file into the hash table
	inode_in_hash = (struct lfs_file_inode_hash*)malloc(sizeof(struct lfs_file_inode_hash));
	strcpy(inode_in_hash->f_name, get_filename(path));
	inode_in_hash->inode_num = inode_blk->ino;
	inode_in_hash->f_size = inode_blk->size;
	struct fuse_context *cxt = fuse_get_context();
	inode_in_hash->owner_num = cxt->uid;  // 调用进程的用户ID
	inode_in_hash->group_num = cxt->gid;
	inode_in_hash->mode = S_IFDIR | mode;  // 目录
	inode_in_hash->is_dir = 1;
	inode_in_hash->subfih=NULL;
	struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);  // 当前时间
	inode_in_hash->atim = ts;
	inode_in_hash->mtim = ts;
	inode_in_hash->ctim = ts;

	// 找到该目录拟在的路径
	char path1[MAXNAMELEN];
	char * lastsplit = strrchr(path, '/');
	strncpy(path1, path, lastsplit-path+1);
	path1[lastsplit-path+1] = '\0';
	
	// ptr_fih指向路径入口
	uint32_t dir_ino;
	if (strcmp(path1, "/")==0)
	{
		HASH_ADD_STR(li->fih, f_name, inode_in_hash);
		dir_ino = -1;
	}
	else
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path1);
		if(ptr_fih==NULL || !ptr_fih->is_dir)
		{
			sem_result = sem_post(&lfs_sem_mkdir);
		    assert(sem_result == 0);
			return -ENOENT;  // 上级目录未找到
		}
		else
		{
			HASH_ADD_STR(ptr_fih->subfih, f_name, inode_in_hash);
			dir_ino = ptr_fih->inode_num;
		}
	}

	// 记录该索引结点的段号和块号
	li->ino_map[inode_blk->ino].seg_num = li->log_head;
	li->ino_map[inode_blk->ino].blk_num = li->cur_seg_blk; 
	li->ino_map[inode_blk->ino].nlink = 1;
	li->ino_map[inode_blk->ino].dir_ino = dir_ino;
	strcpy(li->ino_map[inode_blk->ino].f_name, inode_in_hash->f_name);
	li->ino_map[inode_blk->ino].is_dir = 1;
	
	// 更新段摘要中内容
	struct lfs_seg_sum *seg_sum = (struct lfs_seg_sum *)(li->cur_seg_buf);
	seg_sum[li->cur_seg_blk].inode_num = inode_blk->ino;
	seg_sum[li->cur_seg_blk].logical_blk = -1;

	// if the in-memory segment is full, write the data to disk
	if( li->cur_seg_blk == MAX_SEG_BLKS -1)
	{
		copy_segmentdata_to_log(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE); 
	}
	else
	{
		li->cur_seg_blk++;  // 当前段的当前块号加1
	}

	sem_result = sem_post(&lfs_sem_mkdir);
	assert(sem_result == 0);
	return 0;
}

/** Remove a directory */
static int lfs_rmdir(const char *path)
{
	// 自己写的
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
	printf("\nWXH LFS RMDIR\npath=\"%s\"", fpath);

    // 在哈希表中查找指定的目录
	if (strcmp(path, "/")==0)
	{
		return -EPERM;  // 删除根目录是不被允许的操作
	}
	
	int sem_result = sem_wait(&lfs_sem_rmdir);
	assert(sem_result == 0);

	struct lfs_file_inode_hash *inode_in_hash = find_inode_by_path_in_hash(path);
	if(inode_in_hash == NULL)
	{
		sem_result = sem_post(&lfs_sem_rmdir);
		assert(sem_result == 0);
		return -ENOENT;  // 指定名称的目录不存在
	}
	
	if (!inode_in_hash->is_dir)
	{
		sem_result = sem_post(&lfs_sem_rmdir);
		assert(sem_result == 0);
		return -ENOTDIR;  // 不是目录
	}

    if (inode_in_hash->subfih != NULL)
	{
		sem_result = sem_post(&lfs_sem_rmdir);
		assert(sem_result == 0);
		return -ENOTEMPTY;  // 目录非空
	}

	if(!checkWriteMod(inode_in_hash->owner_num, inode_in_hash->group_num, inode_in_hash->mode))
	{
		sem_result = sem_post(&lfs_sem_rmdir);
		assert(sem_result == 0);
		return -EACCES;  // 权限不符
	}

	// 从哈希表中删除该目录结点
	// 找到该目录所在的路径
	char path1[MAXNAMELEN];
	char * lastsplit = strrchr(path, '/');
	strncpy(path1, path, lastsplit-path+1);
	path1[lastsplit-path+1] = '\0';
	
	// ptr_fih指向路径入口
	if (strcmp(path1, "/")==0)
	{
		HASH_DEL(li->fih, inode_in_hash);
	}
	else
	{
		struct lfs_file_inode_hash *ptr_fih = find_inode_by_path_in_hash(path1);
		if(ptr_fih==NULL)
		{
			sem_result = sem_post(&lfs_sem_rmdir);
		    assert(sem_result == 0);
			return -ENOENT;  // 上级目录未找到
		}
		else
		{
			HASH_DEL(ptr_fih->subfih, inode_in_hash);
		}
	}

    // 更新索引结点表中条目
	if (li->ino_map[inode_in_hash->inode_num].nlink>1)
	{
		li->ino_map[inode_in_hash->inode_num].nlink--;
	}
	else
	{
		li->n_inode--;
		li->ino_map[inode_in_hash->inode_num].seg_num = -1;
		li->ino_map[inode_in_hash->inode_num].blk_num = -1;
		li->ino_map[inode_in_hash->inode_num].nlink = 0;
		li->ino_map[inode_in_hash->inode_num].dir_ino = -2;
		strcpy(li->ino_map[inode_in_hash->inode_num].f_name, "");
		li->ino_map[inode_in_hash->inode_num].is_dir = 0;
	}

	sem_result = sem_post(&lfs_sem_rmdir);
	assert(sem_result == 0);
	return 0;
}

/** Synchronize directory contents
If the datasync parameter is non-zero, then only the user data should be flushed, not the meta data*/
static int lfs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi)
{
	// 自己写
	char fpath[PATH_MAX];
    lfs_fullpath(fpath, path);
    printf("\nWXH FSYNCDIR\npath=\"%s\", datasync=%d, fi=0x%08lx\n", fpath, datasync, (unsigned long)fi);

	int sem_result = sem_wait(&lfs_sem_fsyncdir);
	assert(sem_result == 0);

	// 将缓冲段的所有内容写入磁盘
	size_t ret1 = pwrite(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);
    // 将元数据写入磁盘，操作成功返回0，否则返回-1
    int ret2 = 0;
	if (datasync==0){
		ret2 = copy_metadata_to_log(li->fd);
	}

	sem_result = sem_post(&lfs_sem_fsyncdir);
	assert(sem_result == 0);

	if(ret1 != SEG_SIZE || ret2<0)
	{
		return -EIO;
	}
	else
	{
		return 0;
	}
}

static int lfs_symlink(const char *from, const char *to)
{
	// TODO
	printf("\nWXH LFS SYMLINK\n");
	return 0;
}

static int lfs_statfs(const char *path, struct statvfs *stbuf)
{
	// TODO
	printf("\nWXH LFS STATFS\n");
	return 0;
}

static int lfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
	// TODO
	printf("\nWXH LFS SETXATTR\n");
	return 0;
}

static int lfs_listxattr(const char *path, char *list, size_t size)
{
	// TODO
	printf("\nWXH LFS LISTXATTR\n");
	return 0;
}

static int lfs_removexattr(const char *path, const char *name)
{
	// TODO
	printf("\nWXH LFS REMOVEXATTR\n");
	return 0;
}

static void lfs_destroy(void *private_data)
{
	// 自己写
	printf("\nWXH LFS DESTROY\n");

	// 将缓冲段的所有内容写入磁盘
	int sem_result = sem_wait(&lfs_sem_destroy);
	assert(sem_result == 0);

	size_t ret1 = pwrite(li->fd, li->cur_seg_buf, SEG_SIZE, li->log_head * SEG_SIZE + SUPERBLKSIZE);

    // 将元数据写入磁盘，操作成功返回0，否则返回-1
    int ret2 = copy_metadata_to_log(li->fd);

	sem_result = sem_post(&lfs_sem_destroy);
	assert(sem_result == 0);

	assert(ret1 == SEG_SIZE && ret2>=0);

	// 销毁所有互斥对象
	sem_destroy(&lfs_sem_getattr);
	sem_destroy(&lfs_sem_opendir);
	sem_destroy(&lfs_sem_readdir);
	sem_destroy(&lfs_sem_open);
	sem_destroy(&lfs_sem_read);
	sem_destroy(&lfs_sem_link);
	sem_destroy(&lfs_sem_rename);
	sem_destroy(&lfs_sem_unlink);
	sem_destroy(&lfs_sem_write);
	sem_destroy(&lfs_sem_fsync);
	sem_destroy(&lfs_sem_chown);
	sem_destroy(&lfs_sem_chmod);
	sem_destroy(&lfs_sem_create);
	sem_destroy(&lfs_sem_mkdir);
	sem_destroy(&lfs_sem_rmdir);
	sem_destroy(&lfs_sem_fsyncdir);
	sem_destroy(&lfs_sem_destroy);

	sem_destroy(&lfs_golbal_sem);
}

static int lfs_lock(const char *path, struct fuse_file_info *fi, int cmd, struct flock *lock)
{
	// TODO
	printf("\nWXH LFS LOCK\n");
	return 0;
}

static int lfs_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi)
{
	// TODO
	printf("\nWXH LFS UTIMENS\n");
	return 0;
}

static int lfs_flock(const char *path, struct fuse_file_info *fi, int op)
{
	// TODO
	printf("\nWXH LFS FLOCK\n");
	return 0;
}

static int lfs_fallocate(const char *path, int mode, off_t offset, off_t length, struct fuse_file_info *fi)
{
	// TODO
	printf("\nWXH LFS FALLOCATE\n");
	return 0;
}

static const struct fuse_operations lfs_oper = {
  .init = lfs_init,
  .getattr = lfs_getattr,
  .unlink = lfs_unlink,
  .open = lfs_open,
  .read = lfs_read,
  .write = lfs_write,
  .readdir = lfs_readdir,
  .create = lfs_create,

  /*自己写*/
  .readlink = lfs_readlink,
  .mknod = lfs_mknod,
  .mkdir = lfs_mkdir,
  .rmdir = lfs_rmdir,
  .symlink = lfs_symlink,
  .rename = lfs_rename,
  .link = lfs_link,
  .chmod = lfs_chmod,
  .chown = lfs_chown,
  .truncate = lfs_truncate,
  .statfs = lfs_statfs,
  .flush = lfs_flush,
  .release = lfs_release,
  .fsync = lfs_fsync,
  .fsyncdir = lfs_fsyncdir,
  .setxattr = lfs_setxattr,
  .getxattr = lfs_getxattr,
  .listxattr = lfs_listxattr,
  .removexattr = lfs_removexattr,
  .opendir = lfs_opendir,
  .releasedir = lfs_releasedir,
  .destroy = lfs_destroy,
  .access = lfs_access,
  .lock = lfs_lock,
  .utimens = lfs_utimens,
  .flock = lfs_flock,
  .fallocate = lfs_fallocate,

  //.write_buf = lfs_write_buf,  // 如果实现了这个方法，执行cp的时候就会调用这个方法而不调用_write了
  //.read_buf = lfs_read_buf,  // 如果实现了这个方法，执行cat的时候就会调用这个方法而不调用_read了
  
  // .poll
  // .ioctl
  // .copy_file_range
  // .lseek
};

int main(int argc, char *argv[])
{
	// 保持和骨架的一致性，放个写日志和记录路径，但这里路径直接只有挂载路径了
	struct private_state* lfs_private_data = (struct private_state *)malloc(sizeof(struct private_state));
	assert(NULL != lfs_private_data);
    lfs_private_data->rootdir = realpath(argv[argc-1], NULL);  // 用于形成完整路径

    // 初始化全局变量li
	li = (struct lfs_global_info*)malloc(sizeof(struct lfs_global_info));

    // 打开用做文件系统持久存储的文件
	// open是linux硬件设备操作函数，O_RDWR表示以可读可写的方式打开
	li->fd = open("./lfslog", O_RDWR);
	assert(li->fd > 0);
    int result_load = load_metadata_from_log(li->fd);
	if (result_load < 0)  // 加载失败则清空
	{
		clear_log();
	}

    // 初始化所有互斥对象
	sem_init(&lfs_golbal_sem, 0, 1);
	sem_init(&lfs_sem_getattr, 0, 1);
	sem_init(&lfs_sem_opendir, 0, 1);
	sem_init(&lfs_sem_readdir, 0, 1);
	sem_init(&lfs_sem_open, 0, 1);
	sem_init(&lfs_sem_read, 0, 1);
	sem_init(&lfs_sem_link, 0, 1);
	sem_init(&lfs_sem_rename, 0, 1);
	sem_init(&lfs_sem_unlink, 0, 1);
	sem_init(&lfs_sem_write, 0, 1);
	sem_init(&lfs_sem_fsync, 0, 1);
	sem_init(&lfs_sem_chown, 0, 1);
	sem_init(&lfs_sem_chmod, 0, 1);
	sem_init(&lfs_sem_create, 0, 1);
	sem_init(&lfs_sem_mkdir, 0, 1);
	sem_init(&lfs_sem_rmdir, 0, 1);
	sem_init(&lfs_sem_fsyncdir, 0, 1);
	sem_init(&lfs_sem_destroy, 0, 1);

    // 控制权转给FUSE
    return fuse_main(argc, argv, &lfs_oper, lfs_private_data);
}
