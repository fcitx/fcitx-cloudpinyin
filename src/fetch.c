/***************************************************************************
 *   Copyright (C) 2012~2012 by CSSlayer                                   *
 *   wengxt@gmail.com                                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <curl/curl.h>

#include <fcitx-config/fcitx-config.h>

#include "cloudpinyin.h"
#include "fetch.h"

static void FetchProcessEvent(FcitxFetchThread* fetch);
static void FetchProcessPendingRequest(FcitxFetchThread* fetch);
static void FetchFinish(FcitxFetchThread* fetch, CurlQueue* queue);

void* FetchThread(void* arg)
{
    FcitxFetchThread* fetch = (FcitxFetchThread*) arg;
    fetch->curlm = curl_multi_init();
    if (fetch->curlm == NULL)
        return NULL;
    curl_multi_setopt(fetch->curlm, CURLMOPT_MAXCONNECTS, MAX_HANDLE);

    while (true) {

        char c;
        while (read(fetch->pipeRecv, &c, sizeof(char)) > 0);

        FetchProcessPendingRequest(fetch);
        FetchProcessEvent(fetch);

        FD_ZERO(&fetch->rfds);
        FD_ZERO(&fetch->wfds);
        FD_ZERO(&fetch->efds);

        FD_SET(fetch->pipeRecv, &fetch->rfds);
        fetch->maxfd = fetch->pipeRecv;

        int maxfd;
        curl_multi_fdset(fetch->curlm,
                         &fetch->rfds,
                         &fetch->wfds,
                         &fetch->efds,
                         &maxfd);

        if (maxfd > fetch->maxfd)
            fetch->maxfd = maxfd;

        struct timeval t, *pt;
        t.tv_sec = 1;
        t.tv_usec = 0;

        /* if we have something to fetch, but maxfd is -1 then we give select a time out */
        if (maxfd < 0 && fetch->queue->next != NULL)
            pt = &t;
        else
            pt = NULL;

        select(fetch->maxfd + 1, &fetch->rfds, &fetch->wfds, &fetch->efds, pt);
    }

    return NULL;
}

void FetchProcessEvent(FcitxFetchThread* fetch)
{
    CURLMcode mcode;
    int still_running;
    do {
        mcode = curl_multi_perform(fetch->curlm, &still_running);
    } while (mcode == CURLM_CALL_MULTI_PERFORM);

    int num_messages = 0;
    CURLMsg* curl_message = curl_multi_info_read(fetch->curlm, &num_messages);
    CurlQueue* queue, *previous;

    while (curl_message != NULL) {
        if (curl_message->msg == CURLMSG_DONE) {
            int curl_result = curl_message->data.result;
            previous = fetch->queue;
            queue = fetch->queue->next;
            while (queue != NULL &&
                   queue->curl != curl_message->easy_handle) {
                previous = queue;
                queue = queue->next;
            }
            if (queue != NULL) {
                curl_multi_remove_handle(fetch->curlm, queue->curl);
                previous->next = queue->next;
                queue->curl_result = curl_result;
                curl_easy_getinfo(queue->curl, CURLINFO_RESPONSE_CODE,
                                  &queue->http_code);
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
        curl_multi_add_handle(fetch->curlm, tail->curl);
        tail = tail->next;
        flag = true;
    }

    if (flag) {
        CURLMcode mcode;
        do {
            mcode = curl_multi_perform(fetch->curlm, &still_running);
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
    queue->next = NULL;
    pthread_mutex_unlock(fetch->finishQueueLock);

    char c = 0;
    write(fetch->pipeNotify, &c, sizeof(char));
}
