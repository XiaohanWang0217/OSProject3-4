#!/bin/sh
cd /mnt/e/wsOS/code

sudo umount lfstest_dir

fusermount -u lfstest_dir

sudo ls -lR
# 列出的内容中没有一堆问号的话，就是成功恢复到原始状态了