
#include <sys/select.h>
#include <unistd.h>
#include <pthread.h>

#include <curl/curl.h>

#include <fcitx-config/fcitx-config.h>

#include "cloudpinyin.h"
#include "fetch.h"

static void FetchProcessEvent(FcitxFetchThread* fetch);
static void FetchProcessPendingRequest(FcitxFetchThread* fetch);
static void FetchFinish(FcitxFetchThread* fetch, CurlQueue* queue);

void FetchThread(void* arg)
{
    FcitxFetchThread* fetch = (FcitxFetchThread*) arg;
    fetch->curlm = curl_multi_init();
    if (fetch->curlm == NULL) {
        return;
    }
    curl_multi_setopt(fetch->curlm, CURLMOPT_MAXCONNECTS, 10l);

    while (true) {
        boolean flag = false;
        char c;
        while (read(fetch->pipeRecv, &c, sizeof(char)) > 0)
            flag = true;

        if (flag) {
            FetchProcessPendingRequest(fetch);
        }

        FetchProcessEvent(fetch);

        FD_ZERO(&fetch->rfds);
        FD_ZERO(&fetch->wfds);
        FD_ZERO(&fetch->efds);

        fetch->maxfd = fetch->pipeRecv;

        int maxfd;
        curl_multi_fdset(fetch->curlm,
                         &fetch->rfds,
                         &fetch->wfds,
                         &fetch->efds,
                         &maxfd);

        if (maxfd > fetch->maxfd)
            fetch->maxfd = maxfd;

        select(fetch->maxfd + 1, &fetch->rfds, &fetch->wfds, &fetch->efds, NULL);
    }
}

void FetchProcessEvent(FcitxFetchThread* fetch)
{
    CURLMcode mcode;
    int still_running;
    do {
        mcode = curl_multi_perform(fetch->curlm, &still_running);
    } while (mcode == CURLM_CALL_MULTI_PERFORM);

    int num_messages = 0;
    CURLMsg* curl_message = curl_multi_info_read(fetch->curlm, &num_messages);;
    CurlQueue* queue, *previous;

    while (curl_message != NULL) {
        if (curl_message->msg == CURLMSG_DONE) {
            int curl_result = curl_message->data.result;
            previous = fetch->queue;
            queue = fetch->queue->next;
            while (queue != NULL &&
                    queue->curl != curl_message->easy_handle)
            {
                previous = queue;
                queue = queue->next;
            }
            if (queue != NULL) {
                curl_multi_remove_handle(fetch->curlm, queue->curl);
                previous->next = queue->next;
                queue->curl_result = curl_result;
                curl_easy_getinfo(queue->curl, CURLINFO_HTTP_CODE, &queue->http_code);
                FetchFinish(fetch, queue);
            }
        }
        curl_message = curl_multi_info_read(fetch->curlm, &num_messages);
    }
}

void FetchProcessPendingRequest(FcitxFetchThread* fetch)
{
    boolean still_running;

    /* pull all item from pending queue and move to fetch queue */
    pthread_mutex_lock(fetch->pendingQueueLock);
    FcitxCloudPinyin *cloudpinyin = fetch->owner;
    CurlQueue* head = cloudpinyin->pendingQueue;
    CurlQueue* tail = fetch->queue;
    while(tail->next)
        tail = tail->next;
    while(head->next) {
        CurlQueue* item = head->next;
        item->next = tail->next;
        tail->next = item;
        head->next = head->next->next;
    }
    pthread_mutex_unlock(fetch->pendingQueueLock);
    /* new item start from here */
    tail = tail->next;
    boolean flag = false;
    while(tail) {
        curl_multi_add_handle(cloudpinyin->curlm, tail->curl);
        tail = tail->next;
        flag = true;
    }

    if (flag) {
        CURLMcode mcode;
        do {
            mcode = curl_multi_perform(cloudpinyin->curlm, &still_running);
        } while (mcode == CURLM_CALL_MULTI_PERFORM);
    }
}

void FetchFinish(FcitxFetchThread* fetch, CurlQueue* queue)
{
    pthread_mutex_lock(fetch->finishQueueLock);

    FcitxCloudPinyin *cloudpinyin = fetch->owner;

    CurlQueue* head = cloudpinyin->finishQueue;
    while(head->next)
        head = head->next;
    head->next = queue;
    pthread_mutex_unlock(fetch->finishQueueLock);

    char c = 0;
    write(fetch->pipeNotify, &c, sizeof(char));
}
