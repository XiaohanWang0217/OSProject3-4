#ifndef LFS_QUEUE_H_
#define LFS_QUEUE_H_

#include<stdio.h>
#include<stdlib.h>
#include "lfs.h"

typedef struct lfs_file_inode_hash* QData;  // 队列中数据元素的类型

// 链队列结点类型
typedef struct QNode
{
    QData data;
    struct QNode *next;
}QNode;

// 链队列
typedef struct Queue
{
    QNode *front;
    QNode *rear;
    int length;  /* 记录链队列长度 */
}Queue;

enum QOP_RESULT
{
    QOP_SUCCESS = 0, QOP_ERROR = -1
};

// 初始化，生成一个带头结点的空队列
int initQueue(Queue* q)
{
    QNode *p = (QNode *)malloc(sizeof(QNode));
    if (p==NULL)
    {
        return QOP_ERROR;
    }
    p->next = NULL;

    q->front = p;
    q->rear = p;
    q->length = 0;

    return QOP_SUCCESS;
}

// 入队
int pushQueue(Queue* q, QData data)
{
    // 生成新结点
    QNode *p = (QNode *)malloc(sizeof(QNode));
    if (p==NULL)
    {
        return QOP_ERROR;
    }
    p->next = NULL;
    p->data = data;
    
    q->rear->next = p;
    q->rear = p;

    q->length ++;

    return QOP_SUCCESS;
}

// 出队
QData popQueue(Queue* q)
{
    if (q->length <= 0)
        return NULL;

    QNode *p = q->front->next;  // 头结点的后一个结点
    q->front->next = p->next;   
    QData data = p->data;
    free(p);
    q->length--;
    if (q->length == 0)
    {
        q->rear = q->front;
    }
    return data;
}

// 判队空
int isEmptyQueue(Queue q)
{
    if (q.length <= 0)
        return 1;
    else
        return 0;
}

// 返回队列长度
int getQueueLength(Queue q)
{
    return q.length;
}

// 销毁队列
void destroyQueue(Queue* q)
{
    if (q != NULL && q->front!=NULL)
    {
        QNode *p;
        while(q->front != NULL)
        {
            p = q->front;
            q->front = p->next;
            free(p);
        }
    }
    return;
}

#endif