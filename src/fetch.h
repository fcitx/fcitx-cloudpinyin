#ifndef FCITX_FETCH_H
#define FCITX_FETCH_H

typedef struct _FcitxFetchThread {
    CURLM* curlm;
    int pipeRecv;
    int pipeNotify;
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    int maxfd;
    CurlQueue* queue;

    pthread_mutex_t* pendingQueueLock;
    pthread_mutex_t* finishQueueLock;

    FcitxCloudPinyin* owner;
} FcitxFetchThread;

void FetchThread(void* arg);

#endif
