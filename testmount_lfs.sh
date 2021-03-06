#!/bin/sh
sudo cp lfslog_blank lfslog
# 必要时清空磁盘中内容

gcc -Wall lfs.c `pkg-config fuse3 --cflags --libs` -o lfs
# -Wall指定生成所有警告信息
# -o指定输出文件名
# `pkg-config fuse3 --cflags --libs`用于把fuse3的头文件路径和库文件列出来，让编译去获取

sudo ./lfs -d lfstest_dir
#sudo ./lfs -o allow_other -d lfstest_dir

# 再开一个terminal

mount |grep lfstest_dir
# 【格式】mount
# 【功能】用于挂载Linux系统外的文件
# -o sync：在同步模式下执行

sudo ls -l lfstest_dir
# 此时会调用lfs_getattr、lfs_getxattr（调用了3次，name=security.selinux、name=system.posix_acl_access、name=system.posix_acl_default）
# lfs_opendir、lfs_readdir、lfs_getattr、lfs_getxattr（name=system.posix_acl_access）、lfs_releasedir

# 直接用linux命令的测试
sudo cp testcases_lfs/test.txt lfstest_dir/test.txt
# 【格式】cp [选项] [源文件或目录] [目标文件或目录]
# 【功能】复制文件或目录
# 此时会调用lfs_getattr(为什么调用了3次？)、lfs_create、lfs_getattr、lfs_getxattr、lfs_write、lfs_flush、lfs_lock、lfs_release

sudo cp testcases_lfs/huge_file.txt lfstest_dir/test_huge_file.txt
sudo cat lfstest_dir/test_huge_file.txt
sudo ls -l lfstest_dir
# 复制大文件

sudo cat lfstest_dir/test.txt
# 【格式】cat 文件名
# 【功能】连接文件并打印到标准输出设备
# 此时会调用lfs_getattr、lfs_open、lfs_read、lfs_getattr、lfs_flush、lfs_lock、lfs_release

cat testcases_lfs/test.txt
# 也能输出文件内容，但不会调用lfs_系列函数

sudo ln -d lfstest_dir/test.txt lfstest_dir/test_txt_hardlink
sudo cat lfstest_dir/test_txt_hardlink
sudo rm lfstest_dir/test_txt_hardlink
# 【格式】ln [选项] [源文件或目录] [目标文件或目录]
# 【功能】为源文件创建链接。Linux中的链接可分硬链接和软链接两种，
# 硬链接的意思是一个档案可以有多个名称，软链接的方式是产生一个特殊的档案，该档案的内容是指向另一个档案的位置。
# 硬链接是存在同一个文件系统中，而软链接可以跨越不同的文件系统。
# 无论硬链接或软链接都不会将原本的档案复制一份，只会占用非常少量的磁盘空间。
# 加-d选项创建的是硬链接
# lfs_getattr（针对test.txt）、lfs_getattr（针对test_txt_hardlink，3次）、echofs_link、lfs_getattr（针对test_txt_hardlink）

sudo cp lfstest_dir/test.txt lfstest_dir/test1.txt
sudo mv lfstest_dir/test1.txt lfstest_dir/test2.txt
sudo cat lfstest_dir/test2.txt
# 【格式】mv [选项] [源文件或目录] [目标文件或目录]
# 【功能】为文件或目录改名，或将文件或目录移入其它位置
# lfs_getattr、lfs_rename

# sudo vi lfstest_dir/test3.txt
# echofs_access、echofs_getattr、echofs_mknod、echofs_open、echofs_flush、echofs_release、echofs_unlink、echofs_write、echofs_truncate、echofs_fsync

sudo rm lfstest_dir/test2.txt
sudo ls -l lfstest_dir
# 【格式】rm [选项] [目标文件或目录]
# lfs_getattr、lfs_unlink

#sudo sed -i 's#abc#replacetext#' mount_dir/testfile.txt
# 【格式】sed -i [替换格式] [文件名]
# 【功能】替换格式为's#原内容#替换后内容#' 或 '行号s#原内容#替换后内容#'
# 出错 sed: couldn't open temporary file mount_dir/sedoQUB25: Permission denied

#sudo echo abcdefg >> mount_dir/testfile.txt
#【格式】echo abcdef>>a.txt
# 向文件追加内容
# 出错 bash: mount_dir/abc.txt: Permission denied

sudo chgrp amanda lfstest_dir/test.txt
sudo ls -l lfstest_dir
# 【格式】chgrp 群组名 文件
# 【功能】用于变更文件或目录的所属群组。该命令允许普通用户改变文件所属的组，只要该用户是该组的一员。
# lfs_getattr、lfs_chown

sudo chown amanda:amanda lfstest_dir/test.txt
sudo ls -l lfstest_dir
# 【格式】chown 用户名[:组] 文件
# 【功能】设置文件所有者和文件关联组。只有超级用户和属于组的文件所有者才能变更文件关联组。非超级用户如需要设置关联组可能需要使用chgrp命令。
# lfs_getattr、lfs_chown

sudo chmod g-x lfstest_dir/test.txt
sudo ls -l lfstest_dir
# 【格式】chmod 权限设定字符串 文件
# 其中权限设字符串的格式为[ugoa…][[+-=][rwxX]…][,…]
# u 表示该文件的拥有者，g 表示与该文件的拥有者属于同一个群体(group)者，o 表示其他以外的人，a 表示这三者皆是。
# + 表示增加权限、- 表示取消权限、= 表示唯一设定权限。
# r 表示可读取，w 表示可写入，x 表示可执行，X 表示只有当该文件是个子目录或者该文件已经被设定过为可执行。
# lfs_getattr、lfs_getxattr（3次）、lfs_opendir、lfs_readdir、lfs_getattr、lfs_getxattr(system.posix_acl_access)、lfs_releasedir、lfs_chmod

sudo mkdir lfstest_dir/testdir
sudo ls -l lfstest_dir
sudo ls -l lfstest_dir/testdir
sudo mkdir lfstest_dir/testdir/newdir
sudo ls -l lfstest_dir
sudo ls -l lfstest_dir/testdir
sudo ls -l lfstest_dir/testdir/newdir
sudo mkdir lfstest_dir/testdir/newdir2
sudo ls -l lfstest_dir/testdir
sudo mkdir lfstest_dir/testdir2
sudo ls -l lfstest_dir
# lfs_getattr、lfs_mkdir、lfs_getattr

sudo cp testcases_lfs/test.txt lfstest_dir/testdir/newdir/test.txt
sudo cat lfstest_dir/testdir/newdir/test.txt

sudo chmod 000 lfstest_dir/test.txt
sudo cat lfstest_dir/test.txt
sudo chmod 777 lfstest_dir/test.txt
sudo cat lfstest_dir/test.txt
# 必要时修改测例
cd testcases_lfs
make
sudo ./lfs_test