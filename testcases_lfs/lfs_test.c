#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>


int main()
{
    char *bufread, *bufwrite, *buf;
    int testfile,testfile2,n1,r;
    int result;

    // 写一些字节到文件中
    /*
    printf("\n==========TESTCASE-1: write==========\n");  
    // 创建文件
    // 依次执行了lfs_getattr、lfs_create、lfs_getattr、lfs_flush
    testfile = open("../lfstest_dir/testfile1.txt",  O_CREAT | O_RDWR);
    if (testfile < 0)
        fprintf(stderr, "error no: %d\n", errno);
    assert(testfile > 0);
    // 写入文件
    // 依次执行了lfs_getattr、lfs_open、lfs_write、lfs_flush、
    bufwrite = "Operating Systems - Fall 2020 Project 3";
    result = write(testfile, bufwrite, strlen(bufwrite));
    printf("\nbytes written = %d\n", result);    
    close(testfile);
    */

    printf("\n==========TESTCASE-2：pread==========\n");
    // 从一个文件中读出
    // 依次执行了lfs_getattr、lfs_open、lfs_read、lfs_flush
    bufread = (char*)malloc(200);
    // 打开文件
    testfile = open("../lfstest_dir/test.txt",  O_RDWR);
    if (testfile < 0)
        fprintf(stderr, "error no: %d\n", errno);
    assert(testfile > 0);
    // 从文件读入
    result = pread(testfile, bufread, 30, 0);
    printf("\nbytes read = %d\n", result);
    close(testfile);
    
    // 写入另一个文件
    testfile2 = open("../lfstest_dir/testfile2.txt",  O_CREAT | O_RDWR);
    if (testfile2 < 0)
        fprintf(stderr, "error no: %d\n", errno);
    assert(testfile2 > 0);
    result = write(testfile2, bufread, strlen(bufread));
    printf("\nbytes written = %d\n", result);

    free(bufread);

    close(testfile2);
    /*
    printf("\n==========TESTCASE-3：unlink==========\n");
    // 删除文件
    // 依次执行lfs_getattr、lfs_unlink、lfs_getattr
    result = unlink("../lfstest_dir/testfile1.txt");
    if (result < 0)
        fprintf(stderr, "error no: %d\n", errno);

    testfile = open("../lfstest_dir/testfile1.txt", O_RDWR);
    if (testfile < 0){
        if(errno == 2)
        {
            fprintf(stderr, "%d : No such file or directory\n", errno);
        }
        else
        {
            fprintf(stderr, "error no: %d\n", errno);
        }
    }
    close(testfile);
    */
}
