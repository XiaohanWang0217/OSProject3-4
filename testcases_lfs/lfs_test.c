#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

int count = 0;

void *mul_thread_test(void * args)
{
    char *bufread;
    int testfile,testfile2;
    int result;
    char filename[100];

    // 每个线程写不同的文件
    sprintf(filename, "../lfstest_dir/testfile%d.txt", count++);
    printf("\n%s\n", filename);

    // 各线程读同一个文件
    // 依次执行了lfs_getattr、lfs_open、lfs_read、lfs_flush
    bufread = (char*)malloc(9000000);
    // 打开文件
    testfile = open("../testcases_lfs/huge_file.txt",  O_RDWR);
    //testfile = open("../lfstest_dir/testfile1.txt",  O_RDWR);
    if (testfile < 0)
        fprintf(stderr, "error no: %d\n", errno);
    assert(testfile > 0);
    // 从文件读入
    result = pread(testfile, bufread, 9000000, 0);
    printf("bytes read for file %s = %d\n", filename, result);
    close(testfile);

    // 写入另一个文件
    testfile2 = open(filename,  O_CREAT | O_RDWR);
    if (testfile2 < 0)
        fprintf(stderr, "error no: %d\n", errno);
    assert(testfile2 > 0);
    result = write(testfile2, bufread, strlen(bufread));
    printf("\nbytes written to file %s = %d\n", filename, result);

    free(bufread);

    close(testfile2);
    return NULL;
}

int main()
{
    char *bufread, *bufwrite, *buf;
    int testfile,testfile2,n1,r;
    int result;

    /*
    // 写一些字节到文件中
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

    /*
    printf("\n==========TESTCASE-2：read==========\n");
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
    */

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
            fprintf(stderr, "file deleted\n");
        }
        else
        {
            fprintf(stderr, "error no: %d\n", errno);
        }
    }
    close(testfile);
    */

    /*
    printf("\n==========TESTCASE-4：多线程==========\n");
    int n_threads = 3;
    pthread_t pt[n_threads];  // 定义线程ID
    
    // 四个参数依次为：线程ID的地址；线程的属性；调用的函数；传入的参数
    // 第三个参数要求形为 void *函数名(void *args){} 形式，函数的参数对应第四个参数
    for (int i=0; i<n_threads; i++)
    {
        pthread_create(&pt[i], NULL, mul_thread_test, &i);
    }

    for (int i=0; i<n_threads; i++)
    {
        pthread_join(pt[i], NULL);
    }
    */
    printf("\n==========TESTCASE-5：同步==========\n");
    sync();

    return 0;
}
