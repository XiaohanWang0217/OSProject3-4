#ifndef LFS_DATA_STUCTURE_H_
#define LFS_DATA_STUCTURE_H_
#define SUPERBLKSIZE 131072
// 按总体要求中提出的块大小为1KB
#define BLKSIZE 1024
// 每段最大的块数
#define MAX_SEG_BLKS 128
// 每个段的容量（字节数）
#define SEG_SIZE MAX_SEG_BLKS*BLKSIZE
// 最大的段数(留了一个段放一个超级块，有点浪费，但。。。)
#define MAX_NUM_SEG 799
// 索引结点中直接块条目数
#define MAX_DIRECT_BLKS_INODE 27
// 索引结点中间接块条目数
#define MAX_UNDIRECT_BLKS_INODE 100
// 间接块中条目数
#define MAX_BLKS_4_UNDIRECT 128
// 最大的索引结点数
#define MAX_INODES	300
// 空闲段数量少于该值时需要进行清理
#define MIN_FREE_SEG_NUM 5
// 一个段中dead的块超过该阈值时就需要进行清理
#define THRESHOLD	(MAX_SEG_BLKS / 2)
// 文件名最大长度
#define MAXNAMELEN	128


// 数据块/间接块的磁盘地址
struct lfs_blk_addr {
	int32_t seg_num;  // 段号
	int32_t blk_num;  // 物理块号
};

// 索引结点块
// 每个文件或目录有且仅有一个索引结点块
struct lfs_inode {
	uint32_t ino;	// 索引结点编号
	uint32_t size;  // 文件大小
	struct lfs_blk_addr direct_blks[MAX_DIRECT_BLKS_INODE];  // 文件直接块地址
	struct lfs_blk_addr undirect_blks[MAX_UNDIRECT_BLKS_INODE];  // 文件间接块地址
};

// 段摘要块
struct lfs_seg_sum {
	int32_t inode_num;	// 索引结点编号
	int32_t logical_blk;	// 逻辑块号（间接块也占用逻辑块号）
};

// 所有文件及目录的基本信息的哈希表
struct lfs_file_inode_hash {
    char f_name[MAXNAMELEN];  // 文件或目录名
    uint32_t inode_num;  // 索引结点编号
	uint32_t f_size;  // 文件大小，对于目录该值为0
    uint32_t owner_num;  // 文件所有者uid
	uint32_t group_num;  // 文件所有者所属组gid
    uint16_t is_dir; // 1则为目录，0为文件
	struct lfs_file_inode_hash *subfih;  // 目录下的内容
	struct timespec atim;  // 文件最后被访问的时间
	struct timespec mtim;  // 文件内容最后被修改的时间
	struct timespec ctim;  // 文件状态改变时间
	mode_t mode;  // 文件权限（chmod命令中用）
	UT_hash_handle hh;  // 哈希句柄
};

// 由索引结点编号找到索引结点物理地址
struct lfs_inode_map_entry {
	int32_t seg_num;  // 段号
	int32_t blk_num;  // 物理块号
	int32_t nlink;  // 硬链接数
	// 用于崩溃恢复的
	char f_name[MAXNAMELEN];  // 文件名或目录名
	int32_t dir_ino;  // 所在目录的索引结点号
	uint16_t is_dir;  // 是否为目录，1表示目录，0表示文件
};

// 总的数据结构
struct lfs_global_info {
	struct lfs_file_inode_hash *fih;  // 根目录下内容的哈希表
	char  *cur_seg_buf;  // 当前缓存段
	uint32_t cur_seg_blk;  // 当前段的当前块
	uint64_t n_inode;  // 当前索引结点总数
	int32_t log_head;  // 当前缓存段的段号
	struct lfs_inode_map_entry ino_map[MAX_INODES];  // 索引结点块物理地址及硬链接数
	uint16_t seg_bitmap[MAX_NUM_SEG];  // 段使用情况表
	uint32_t threshold;  // 用于决定段是否需要清理的阈值
	int64_t fd;  // 指向作为外存的大文件
};

// 块的类型
enum BLK_TYPE{
	DIRECT_DATA_BLK = 1, UNDIRECT_DATA_BLK, UNDIRECT_BLK
};

struct private_state {
    char *rootdir;
};

#endif