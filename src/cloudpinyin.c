/***************************************************************************
 *   Copyright (C) 2011~2012 by CSSlayer                                   *
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

#include "config.h"

#include <errno.h>
#include <iconv.h>
#include <unistd.h>

#include <curl/curl.h>
#include <fcntl.h>

#include <fcitx/fcitx.h>
#include <fcitx/module.h>
#include <fcitx/hook.h>
#include <fcitx-utils/log.h>
#include <fcitx/candidate.h>
#include <fcitx-config/xdg.h>
#include <fcitx/module/pinyin/fcitx-pinyin.h>

#include "cloudpinyin.h"
#include "fetch.h"
#include "parse.h"

DEFINE_GET_ADDON("fcitx-sunpinyin", SunPinyin)
DEFINE_GET_ADDON("fcitx-libpinyin", LibPinyin)
DEFINE_GET_ADDON("fcitx-sogoupinyin", SogouPinyin)
DEFINE_GET_AND_INVOKE_FUNC(SunPinyin, GetFullPinyin, 0)
DEFINE_GET_AND_INVOKE_FUNC(SunPinyin, AddWord, 1)

// Maybe not the right name, but doesn't matter....
DEFINE_GET_AND_INVOKE_FUNC(LibPinyin, AddWord, 0)
DEFINE_GET_AND_INVOKE_FUNC(SogouPinyin, AddWord, 0)

#define CHECK_VALID_IM (im &&                                 \
                        strcmp(im->langCode, "zh_CN") == 0 && \
                        (strcmp(im->uniqueName, "pinyin") == 0 || \
                        strcmp(im->uniqueName, "pinyin-libpinyin") == 0 || \
                        strcmp(im->uniqueName, "shuangpin-libpinyin") == 0 || \
                        strcmp(im->uniqueName, "googlepinyin") == 0 || \
                        strcmp(im->uniqueName, "sunpinyin") == 0 || \
                        strcmp(im->uniqueName, "shuangpin") == 0 || \
                        strcmp(im->uniqueName, "sogou-pinyin") == 0))

#define CLOUDPINYIN_CHECK_PAGE_NUMBER 3

typedef struct _CloudCandWord {
    boolean filled;
    uint64_t timestamp;
} CloudCandWord;

typedef struct _CloudPinyinEngine {
    const char* RequestKey;
    const char* RequestPinyin;
    void (*ParseKey)(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
    char* (*ParsePinyin)(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
} CloudPinyinEngine;

static void* CloudPinyinCreate(FcitxInstance* instance);
static void CloudPinyinSetFD(void* arg);
static void CloudPinyinProcessEvent(void* arg);
static void CloudPinyinDestroy(void* arg);
static void CloudPinyinReloadConfig(void* arg);
static void CloudPinyinAddCandidateWord(void* arg);
static void CloudPinyinRequestKey(FcitxCloudPinyin* cloudpinyin);
static void CloudPinyinAddInputRequest(FcitxCloudPinyin* cloudpinyin, const char* strPinyin);
static void CloudPinyinHandleRequest(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
static size_t CloudPinyinWriteFunction(char *ptr, size_t size, size_t nmemb, void *userdata);
static CloudPinyinCache* CloudPinyinCacheLookup(FcitxCloudPinyin* cloudpinyin, const char* pinyin);
static CloudPinyinCache* CloudPinyinAddToCache(FcitxCloudPinyin* cloudpinyin, const char* pinyin, char* string);
static INPUT_RETURN_VALUE CloudPinyinGetCandWord(void* arg, FcitxCandidateWord* candWord);
static void _CloudPinyinAddCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin);
static void CloudPinyinFillCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin);
static boolean LoadCloudPinyinConfig(FcitxCloudPinyinConfig* fs);
static void SaveCloudPinyinConfig(FcitxCloudPinyinConfig* fs);
static char *GetCurrentString(FcitxCloudPinyin* cloudpinyin,
                              char **ascii_part);
static void CloudPinyinHookForNewRequest(void* arg);
static CURL* CloudPinyinGetFreeCurlHandle(FcitxCloudPinyin* cloudpinyin);
static void CloudPinyinReleaseCurlHandle(FcitxCloudPinyin* cloudpinyin,
                                         CURL* curl);

CloudPinyinEngine engine[4] =
{
    {
        "http://web.pinyin.sogou.com/web_ime/patch.php",
        "http://web.pinyin.sogou.com/api/py?key=%s&query=%s",
        SogouParseKey,
        SogouParsePinyin
    },
    {
        "http://ime.qq.com/fcgi-bin/getkey",
        "http://ime.qq.com/fcgi-bin/getword?key=%s&q=%s",
        QQParseKey,
        QQParsePinyin
    },
    {
        NULL,
        "https://www.google.com/inputtools/request?ime=pinyin&text=%s",
        NULL,
        GoogleParsePinyin
    },
    {
        NULL,
        "http://olime.baidu.com/py?py=%s&rn=0&pn=1&ol=1",
        NULL,
        BaiduParsePinyin
    }
};


CONFIG_DESC_DEFINE(GetCloudPinyinConfigDesc, "fcitx-cloudpinyin.desc")

FCITX_DEFINE_PLUGIN(fcitx_cloudpinyin, module, FcitxModule) = {
    .Create = CloudPinyinCreate,
    .Destroy = CloudPinyinDestroy,
    .SetFD = CloudPinyinSetFD,
    .ProcessEvent = CloudPinyinProcessEvent,
    .ReloadConfig = CloudPinyinReloadConfig
};

static uint64_t
CloudGetTimeStamp()
{
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    return (((uint64_t)current_time.tv_sec * 1000)
            + (current_time.tv_usec / 1000));
}

static void
CloudSetClientPreedit(FcitxCloudPinyin *cloudpinyin, const char *str)
{
    FcitxInputState *input = FcitxInstanceGetInputState(cloudpinyin->owner);
    FcitxMessages *message = FcitxInputStateGetClientPreedit(input);
    char *py;
    char *string = GetCurrentString(cloudpinyin, &py);
    FcitxMessagesSetMessageCount(message, 0);
    if (py) {
        *py = '\0';
        FcitxMessagesAddMessageAtLast(message, MSG_INPUT, "%s%s", string, str);
    } else {
        FcitxMessagesAddMessageAtLast(message, MSG_INPUT, "%s", str);
    }
    fcitx_utils_free(string);
    FcitxInstanceUpdateClientSideUI(
        cloudpinyin->owner, FcitxInstanceGetCurrentIC(cloudpinyin->owner));
}

void* CloudPinyinCreate(FcitxInstance* instance)
{
    FcitxCloudPinyin *cloudpinyin = fcitx_utils_new(FcitxCloudPinyin);
    bindtextdomain("fcitx-cloudpinyin", LOCALEDIR);
    cloudpinyin->owner = instance;
    int pipe1[2];
    int pipe2[2];

    if (!LoadCloudPinyinConfig(&cloudpinyin->config))
    {
        free(cloudpinyin);
        return NULL;
    }

    if (pipe(pipe1) < 0)
    {
        free(cloudpinyin);
        return NULL;
    }

    if (pipe(pipe2) < 0) {
        close(pipe1[0]);
        close(pipe1[1]);
        free(cloudpinyin);
        return NULL;
    }

    cloudpinyin->pipeRecv = pipe1[0];
    cloudpinyin->pipeNotify = pipe2[1];

    fcntl(pipe1[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe1[1], F_SETFL, O_NONBLOCK);
    fcntl(pipe2[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe2[1], F_SETFL, O_NONBLOCK);

    cloudpinyin->pendingQueue = fcitx_utils_malloc0(sizeof(CurlQueue));
    cloudpinyin->finishQueue = fcitx_utils_malloc0(sizeof(CurlQueue));
    pthread_mutex_init(&cloudpinyin->pendingQueueLock, NULL);
    pthread_mutex_init(&cloudpinyin->finishQueueLock, NULL);

    FcitxFetchThread* fetch = fcitx_utils_malloc0(sizeof(FcitxFetchThread));
    cloudpinyin->fetch = fetch;
    fetch->owner = cloudpinyin;
    fetch->pipeRecv = pipe2[0];
    fetch->pipeNotify = pipe1[1];
    fetch->pendingQueueLock = &cloudpinyin->pendingQueueLock;
    fetch->finishQueueLock = &cloudpinyin->finishQueueLock;
    fetch->queue = fcitx_utils_malloc0(sizeof(CurlQueue));

    FcitxIMEventHook hook;
    hook.arg = cloudpinyin;
    hook.func = CloudPinyinAddCandidateWord;

    FcitxInstanceRegisterUpdateCandidateWordHook(instance, hook);

    hook.arg = cloudpinyin;
    hook.func = CloudPinyinHookForNewRequest;

    FcitxInstanceRegisterResetInputHook(instance, hook);
    FcitxInstanceRegisterInputFocusHook(instance, hook);
    FcitxInstanceRegisterInputUnFocusHook(instance, hook);
    FcitxInstanceRegisterTriggerOnHook(instance, hook);

    pthread_create(&cloudpinyin->pid, NULL, FetchThread, fetch);

    CloudPinyinRequestKey(cloudpinyin);

    return cloudpinyin;
}

CURL* CloudPinyinGetFreeCurlHandle(FcitxCloudPinyin* cloudpinyin)
{
    int i = 0;
    for (i = 0; i < MAX_HANDLE; i ++) {
        if (!cloudpinyin->freeList[i].used) {
            cloudpinyin->freeList[i].used = true;
            if (cloudpinyin->freeList[i].curl == NULL) {
                cloudpinyin->freeList[i].curl = curl_easy_init();
            }
            return cloudpinyin->freeList[i].curl;
        }
    }
    /* return a stalled handle */
    return curl_easy_init();
}

void CloudPinyinReleaseCurlHandle(FcitxCloudPinyin* cloudpinyin, CURL* curl)
{
    if (curl == NULL)
        return;
    int i = 0;
    for (i = 0; i < MAX_HANDLE; i ++) {
        if (cloudpinyin->freeList[i].curl == curl) {
            cloudpinyin->freeList[i].used = false;
            return;
        }
    }
    /* if handle is stalled, free it */
    curl_easy_cleanup(curl);
}


void CloudPinyinAddCandidateWord(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    FcitxIM* im = FcitxInstanceGetCurrentIM(cloudpinyin->owner);
    FcitxInputState* input = FcitxInstanceGetInputState(cloudpinyin->owner);

    if (cloudpinyin->initialized == false)
        return;

    /* check whether the current im is pinyin */
    if (CHECK_VALID_IM)
    {
        /* there is something pending input */
        if (FcitxInputStateGetRawInputBufferSize(input) >= cloudpinyin->config.iMinimumPinyinLength)
        {
            char* strToFree = NULL, *inputString;
            strToFree = GetCurrentString(cloudpinyin, &inputString);

            if (inputString) {
                CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, inputString);
                FcitxLog(DEBUG, "%s", inputString);
                if (cacheEntry == NULL)
                    CloudPinyinAddInputRequest(cloudpinyin, inputString);
                _CloudPinyinAddCandidateWord(cloudpinyin, inputString);
            }
            if (strToFree)
                free(strToFree);
        }
    }

    return;
}

void CloudPinyinRequestKey(FcitxCloudPinyin* cloudpinyin)
{
    if (cloudpinyin->isrequestkey)
        return;

    cloudpinyin->isrequestkey = true;
    if (engine[cloudpinyin->config.source].RequestKey == NULL)
    {
        cloudpinyin->initialized = true;
        cloudpinyin->key[0] = '\0';
        cloudpinyin->isrequestkey = false;
        return;
    }

    CURL* curl = CloudPinyinGetFreeCurlHandle(cloudpinyin);
    if (!curl)
        return;
    CurlQueue* queue = fcitx_utils_malloc0(sizeof(CurlQueue)), *head = cloudpinyin->pendingQueue;
    queue->curl = curl;
    queue->next = NULL;
    queue->type = RequestKey;
    queue->source = cloudpinyin->config.source;

    curl_easy_setopt(curl, CURLOPT_URL, engine[cloudpinyin->config.source].RequestKey);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, queue);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CloudPinyinWriteFunction);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20l);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1l);

    /* push into pending queue */
    pthread_mutex_lock(&cloudpinyin->pendingQueueLock);
    while (head->next != NULL)
        head = head->next;
    head->next = queue;
    pthread_mutex_unlock(&cloudpinyin->pendingQueueLock);

    char c = 0;
    write(cloudpinyin->pipeNotify, &c, sizeof(char));
}



void CloudPinyinSetFD(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    FcitxInstance* instance = cloudpinyin->owner;
    int maxfd = cloudpinyin->pipeRecv;
    FD_SET(maxfd, FcitxInstanceGetReadFDSet(instance));
    if (maxfd > FcitxInstanceGetMaxFD(instance))
        FcitxInstanceSetMaxFD(instance, maxfd);
}

void CloudPinyinProcessEvent(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    FcitxInstance* instance = cloudpinyin->owner;
    if (!FD_ISSET(cloudpinyin->pipeRecv, FcitxInstanceGetReadFDSet(instance)))
        return;

    char c;
    while (read(cloudpinyin->pipeRecv, &c, sizeof(char)) > 0);
    pthread_mutex_lock(&cloudpinyin->finishQueueLock);
    CurlQueue* queue;
    queue = cloudpinyin->finishQueue;
    /* this queue header is empty, so the check condition is "next" not null */
    while (queue->next != NULL)
    {
        /* remove pivot from queue, thus pivot need to be free'd in HandleRequest */
        CurlQueue* pivot = queue->next;
        queue->next = queue->next->next;
        CloudPinyinHandleRequest(cloudpinyin, pivot);
    }
    pthread_mutex_unlock(&cloudpinyin->finishQueueLock);
}

void CloudPinyinDestroy(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    char c = 1;
    write(cloudpinyin->pipeNotify, &c, sizeof(char));
    pthread_join(cloudpinyin->pid, NULL);
    pthread_mutex_destroy(&cloudpinyin->pendingQueueLock);
    pthread_mutex_destroy(&cloudpinyin->finishQueueLock);
    while (cloudpinyin->cache)
    {
        CloudPinyinCache* head = cloudpinyin->cache;
        HASH_DEL(cloudpinyin->cache, cloudpinyin->cache);
        free(head->pinyin);
        free(head->str);
        free(head);
    }

    close(cloudpinyin->pipeRecv);
    close(cloudpinyin->pipeNotify);

    close(cloudpinyin->fetch->pipeRecv);
    close(cloudpinyin->fetch->pipeNotify);
    int i = 0;
    for (i = 0; i < MAX_HANDLE; i ++) {
        if (cloudpinyin->freeList[i].curl) {
            curl_easy_cleanup(cloudpinyin->freeList[i].curl);
        }
    }

    curl_multi_cleanup(cloudpinyin->fetch->curlm);
#define _FREE_QUEUE(NAME) \
    while(NAME) { \
        CurlQueue* queue = NAME; \
        NAME = NAME->next; \
        fcitx_utils_free(queue->str); \
        fcitx_utils_free(queue->pinyin); \
        free(queue); \
    }
    _FREE_QUEUE(cloudpinyin->pendingQueue)
    _FREE_QUEUE(cloudpinyin->finishQueue)
    _FREE_QUEUE(cloudpinyin->fetch->queue)
    FcitxConfigFree(&cloudpinyin->config.config);
    free(cloudpinyin->fetch);
    free(cloudpinyin);
}

void CloudPinyinReloadConfig(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    CloudPinyinSource previousSource = cloudpinyin->config.source;
    LoadCloudPinyinConfig(&cloudpinyin->config);
    if (previousSource != cloudpinyin->config.source)
    {
        cloudpinyin->initialized = false;
        cloudpinyin->key[0] = '\0';
    }
}

void CloudPinyinAddInputRequest(FcitxCloudPinyin* cloudpinyin, const char* strPinyin)
{
    CURL* curl = CloudPinyinGetFreeCurlHandle(cloudpinyin);
    if (!curl)
        return;
    CurlQueue* queue = fcitx_utils_malloc0(sizeof(CurlQueue)), *head = cloudpinyin->pendingQueue;
    queue->curl = curl;
    queue->next = NULL;
    queue->type = RequestPinyin;
    queue->pinyin = strdup(strPinyin);
    queue->source = cloudpinyin->config.source;
    char* urlstring = curl_escape(strPinyin, strlen(strPinyin));
    char *url = NULL;
    if (engine[cloudpinyin->config.source].RequestKey)
        asprintf(&url, engine[cloudpinyin->config.source].RequestPinyin, cloudpinyin->key, urlstring);
    else
        asprintf(&url, engine[cloudpinyin->config.source].RequestPinyin, urlstring);
    curl_free(urlstring);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, queue);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CloudPinyinWriteFunction);

    free(url);

    /* push into pending queue */
    pthread_mutex_lock(&cloudpinyin->pendingQueueLock);
    while (head->next != NULL)
        head = head->next;
    head->next = queue;
    pthread_mutex_unlock(&cloudpinyin->pendingQueueLock);

    char c = 0;
    write(cloudpinyin->pipeNotify, &c, sizeof(char));
}

void CloudPinyinHandleRequest(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    if (queue->type == RequestKey)
    {
        cloudpinyin->isrequestkey = false;
        if (queue->source != cloudpinyin->config.source)
            return;

        if (queue->http_code == 200)
        {
            if (engine[cloudpinyin->config.source].ParseKey)
                engine[cloudpinyin->config.source].ParseKey(cloudpinyin, queue);
        }
    }
    else if (queue->type == RequestPinyin)
    {
        if (queue->http_code == 200 && cloudpinyin->config.source == queue->source)
        {
            char *realstring = engine[cloudpinyin->config.source].ParsePinyin(cloudpinyin, queue);
            if (realstring)
            {
                CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, queue->pinyin);
                if (cacheEntry == NULL)
                    cacheEntry = CloudPinyinAddToCache(cloudpinyin, queue->pinyin, realstring);

                FcitxIM* im = FcitxInstanceGetCurrentIM(cloudpinyin->owner);

                char* strToFree = NULL, *inputString;
                strToFree = GetCurrentString(cloudpinyin, &inputString);

                if (inputString) {
                    FcitxLog(DEBUG, "fill: %s %s", inputString, queue->pinyin);
                    if (strcmp(inputString, queue->pinyin) == 0)
                    {
                        if (CHECK_VALID_IM)
                        {
                            CloudPinyinFillCandidateWord(cloudpinyin, inputString);
                        }
                    }
                }
                if (strToFree)
                    free(strToFree);
                free(realstring);
            }
        }

        if (queue->http_code != 200)
        {
            cloudpinyin->errorcount ++;
            if (cloudpinyin->errorcount > MAX_ERROR)
            {
                cloudpinyin->initialized = false;
                cloudpinyin->key[0] = '\0';
                cloudpinyin->errorcount = 0;
            }
        }
    }
    CloudPinyinReleaseCurlHandle(cloudpinyin, queue->curl);
    fcitx_utils_free(queue->str);
    fcitx_utils_free(queue->pinyin);
    free(queue);
}

size_t CloudPinyinWriteFunction(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlQueue* queue = (CurlQueue*) userdata;

    size_t realsize = size * nmemb;
    /*
     * We know that it isn't possible to overflow during multiplication if
     * neither operand uses any of the most significant half of the bits in
     * a size_t.
     */

    if ((unsigned long long)((nmemb | size) &
                             ((unsigned long long)SIZE_MAX << (sizeof(size_t) << 2))) &&
            (realsize / size != nmemb))
        return 0;

    if (SIZE_MAX - queue->size - 1 < realsize)
        realsize = SIZE_MAX - queue->size - 1;

    if (queue->str != NULL)
        queue->str = realloc(queue->str, queue->size + realsize + 1);
    else
        queue->str = fcitx_utils_malloc0(realsize + 1);

    if (queue->str != NULL) {
        memcpy(&(queue->str[queue->size]), ptr, realsize);
        queue->size += realsize;
        queue->str[queue->size] = '\0';
    }
    return realsize;
}

CloudPinyinCache* CloudPinyinCacheLookup(FcitxCloudPinyin* cloudpinyin, const char* pinyin)
{
    CloudPinyinCache* cacheEntry = NULL;
    HASH_FIND_STR(cloudpinyin->cache, pinyin, cacheEntry);
    return cacheEntry;
}

CloudPinyinCache* CloudPinyinAddToCache(FcitxCloudPinyin* cloudpinyin, const char* pinyin, char* string)
{
    CloudPinyinCache* cacheEntry = fcitx_utils_malloc0(sizeof(CloudPinyinCache));
    cacheEntry->pinyin = strdup(pinyin);
    cacheEntry->str = strdup(string);
    HASH_ADD_KEYPTR(hh, cloudpinyin->cache, cacheEntry->pinyin, strlen(cacheEntry->pinyin), cacheEntry);

    /* if there is too much cached, remove the first one, though LRU might be a better algorithm */
    if (HASH_COUNT(cloudpinyin->cache) > MAX_CACHE_ENTRY)
    {
        CloudPinyinCache* head = cloudpinyin->cache;
        HASH_DEL(cloudpinyin->cache, cloudpinyin->cache);
        free(head->pinyin);
        free(head->str);
        free(head);
    }
    return cacheEntry;
}

void _CloudPinyinAddCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin)
{
    CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, pinyin);
    FcitxInputState* input = FcitxInstanceGetInputState(cloudpinyin->owner);
    FcitxCandidateWordList* candList = FcitxInputStateGetCandidateList(input);

    int order = (cloudpinyin->config.iCandidateOrder <= 2) ?
        1 : (cloudpinyin->config.iCandidateOrder - 1);

    if (cacheEntry) {
        FcitxCandidateWord* cand;
        /* only check the first three page */
        int pagesize = FcitxCandidateWordGetPageSize(candList);
        int size = pagesize * CLOUDPINYIN_CHECK_PAGE_NUMBER;
        int i;
        if (cloudpinyin->config.iCandidateOrder <= 1) {
            order = 0;
        }
        for (i = 0;i < size &&
                 (cand = FcitxCandidateWordGetByTotalIndex(candList, i));i++) {
            if (strcmp(cand->strWord, cacheEntry->str) == 0) {
                if (i > order && i >= pagesize) {
                    FcitxCandidateWordMoveByWord(candList, cand, order);
                    if (order == 0) {
                        CloudSetClientPreedit(cloudpinyin, cacheEntry->str);
                    }
                }
                return;
            }
        }
        if (order == 0) {
            CloudSetClientPreedit(cloudpinyin, cacheEntry->str);
        }
    }

    FcitxCandidateWord candWord;
    CloudCandWord* cloudCand = fcitx_utils_malloc0(sizeof(CloudCandWord));
    if (cacheEntry) {
        cloudCand->filled = true;
        cloudCand->timestamp = 0;
        candWord.strWord = strdup(cacheEntry->str);
    } else {
        cloudCand->filled = false;
        cloudCand->timestamp = CloudGetTimeStamp();
        candWord.strWord = strdup("..");
    }

    candWord.callback = CloudPinyinGetCandWord;
    candWord.owner = cloudpinyin;
    candWord.priv = cloudCand;
    candWord.wordType = MSG_TIPS;
    if (cloudpinyin->config.bDontShowSource)
        candWord.strExtra = NULL;
    else {
        candWord.strExtra = strdup(_(" (via cloud)"));
        candWord.extraType = MSG_TIPS;
    }

    FcitxCandidateWordInsert(candList, &candWord, order);
}

#define LOADING_TIME_QUICK_THRESHOLD 300
#define DUP_PLACE_HOLDER "\xe2\x98\xba"

void CloudPinyinFillCandidateWord(FcitxCloudPinyin* cloudpinyin,
                                  const char* pinyin)
{
    CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, pinyin);
    FcitxInputState* input = FcitxInstanceGetInputState(cloudpinyin->owner);
    FcitxCandidateWordList* candList = FcitxInputStateGetCandidateList(input);
    if (cacheEntry) {
        int cloudidx;
        FcitxCandidateWord *candWord;
        for (cloudidx = 0;
             (candWord = FcitxCandidateWordGetByTotalIndex(candList, cloudidx));
             cloudidx++) {
            if (candWord->owner == cloudpinyin)
                break;
        }

        if (candWord == NULL)
            return;

        CloudCandWord* cloudCand = candWord->priv;
        if (cloudCand->filled)
            return;

        FcitxCandidateWord *cand;
        int i;
        int pagesize = FcitxCandidateWordGetPageSize(candList);
        int size = pagesize * CLOUDPINYIN_CHECK_PAGE_NUMBER;
        for (i = 0;i < size &&
                 (cand = FcitxCandidateWordGetByTotalIndex(candList, i));i++) {
            if (strcmp(cand->strWord, cacheEntry->str) == 0) {
                FcitxCandidateWordRemove(candList, candWord);
                /* if cloud word is not on the first page.. impossible */
                if (cloudidx < pagesize) {
                    /* if the duplication before cloud word */
                    if (i < cloudidx) {
                        if (CloudGetTimeStamp() - cloudCand->timestamp
                            > LOADING_TIME_QUICK_THRESHOLD) {
                            FcitxCandidateWordInsertPlaceHolder(candList, cloudidx);
                            FcitxCandidateWord* placeHolder = FcitxCandidateWordGetByTotalIndex(candList, cloudidx);
                            if (placeHolder && placeHolder->strWord == NULL)
                                placeHolder->strWord = strdup(DUP_PLACE_HOLDER);
                        }
                    } else {
                        if (i >= pagesize) {
                            FcitxCandidateWordMove(candList, i - 1, cloudidx);
                        } else {
                            if (CloudGetTimeStamp() - cloudCand->timestamp
                                > LOADING_TIME_QUICK_THRESHOLD) {
                                FcitxCandidateWordInsertPlaceHolder(candList, cloudidx);
                                FcitxCandidateWord* placeHolder = FcitxCandidateWordGetByTotalIndex(candList, cloudidx);
                                if (placeHolder && placeHolder->strWord == NULL)
                                    placeHolder->strWord = strdup(DUP_PLACE_HOLDER);
                            }
                        }
                    }
                }
                FcitxUIUpdateInputWindow(cloudpinyin->owner);
                candWord = NULL;
                break;
            }
        }

        if (candWord) {
            if (cloudCand->filled == false) {
                cloudCand->filled = true;
                free(candWord->strWord);
                candWord->strWord = strdup(cacheEntry->str);
                if (cloudpinyin->config.iCandidateOrder <= 1 &&
                    (CloudGetTimeStamp() - cloudCand->timestamp
                     <= LOADING_TIME_QUICK_THRESHOLD)) {
                    FcitxCandidateWordMoveByWord(candList, candWord, 0);
                    CloudSetClientPreedit(cloudpinyin, cacheEntry->str);
                }
                FcitxUIUpdateInputWindow(cloudpinyin->owner);
            }
        }
    }
}

INPUT_RETURN_VALUE CloudPinyinGetCandWord(void* arg, FcitxCandidateWord* candWord)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    CloudCandWord* cloudCand = candWord->priv;
    FcitxInputState* input = FcitxInstanceGetInputState(cloudpinyin->owner);
    if (cloudCand->filled)
    {
        char *py;
        char *string = GetCurrentString(cloudpinyin, &py);
        if (py) {
            *py = 0;

            snprintf(FcitxInputStateGetOutputString(input),
                     MAX_USER_INPUT, "%s%s", string, candWord->strWord);

            FcitxIM* im = FcitxInstanceGetCurrentIM(cloudpinyin->owner);
            if (im) {
                char *output_string = FcitxInputStateGetOutputString(input);
                FCITX_DEF_MODULE_ARGS(args, output_string);
                if (strcmp(im->uniqueName, "sunpinyin") == 0) {
                    FcitxSunPinyinInvokeAddWord(cloudpinyin->owner, args);
                } else if (strcmp(im->uniqueName, "shuangpin") == 0 ||
                           strcmp(im->uniqueName, "pinyin") == 0) {
                    FcitxPinyinInvokeAddUserPhrase(cloudpinyin->owner, args);
                } else if (strcmp(im->uniqueName, "pinyin-libpinyin") == 0 ||
                           strcmp(im->uniqueName, "shuangpin-libpinyin") == 0) {
                    FcitxLibPinyinInvokeAddWord(cloudpinyin->owner, args);
                }
                else if (strcmp(im->uniqueName, "sogou-pinyin") == 0)
                {
                    FcitxSogouPinyinInvokeAddWord(cloudpinyin->owner, args);
                }
            }
        }
        if (string)
            free(string);
        return IRV_COMMIT_STRING;
    } else {
        return IRV_DO_NOTHING;
    }
}


/**
 * @brief Load the config file for fcitx-cloudpinyin
 *
 * @param Bool is reload or not
 **/
boolean LoadCloudPinyinConfig(FcitxCloudPinyinConfig* fs)
{
    FcitxConfigFileDesc *configDesc = GetCloudPinyinConfigDesc();
    if (configDesc == NULL)
        return false;

    FILE *fp = FcitxXDGGetFileUserWithPrefix("conf", "fcitx-cloudpinyin.config", "r", NULL);

    if (!fp)
    {
        if (errno == ENOENT)
            SaveCloudPinyinConfig(fs);
    }
    FcitxConfigFile *cfile = FcitxConfigParseConfigFileFp(fp, configDesc);
    FcitxCloudPinyinConfigConfigBind(fs, cfile, configDesc);
    FcitxConfigBindSync(&fs->config);

    if (fp)
        fclose(fp);

    return true;
}

/**
 * @brief Save the config
 *
 * @return void
 **/
void SaveCloudPinyinConfig(FcitxCloudPinyinConfig* fs)
{
    FcitxConfigFileDesc *configDesc = GetCloudPinyinConfigDesc();
    FILE *fp = FcitxXDGGetFileUserWithPrefix("conf", "fcitx-cloudpinyin.config", "w", NULL);
    FcitxConfigSaveConfigFileFp(fp, &fs->config, configDesc);
    if (fp)
        fclose(fp);
}

char *GetCurrentString(FcitxCloudPinyin* cloudpinyin, char **ascii_part)
{
    FcitxIM* im = FcitxInstanceGetCurrentIM(cloudpinyin->owner);
    if (!im) {
        *ascii_part = NULL;
        return NULL;
    }
    FcitxInputState* input = FcitxInstanceGetInputState(cloudpinyin->owner);
    char* string = FcitxUIMessagesToCString(FcitxInputStateGetPreedit(input));
    char p[MAX_USER_INPUT + 1], *pinyin, *lastpos;
    pinyin = fcitx_utils_get_ascii_part(string);
    lastpos = pinyin;
    boolean endflag;
    int hzlength = pinyin - string;
    size_t plength = hzlength;
    strncpy(p, string, hzlength);
    p[hzlength] = '\0';
    do {
        endflag = (*pinyin != '\0');
        if (*pinyin == ' ' || *pinyin == '\'' || *pinyin == '\0') {
            *pinyin = 0;

            if (*lastpos != '\0') {
                char* result = NULL;
                boolean isshuangpin = false;
                if (strcmp(im->uniqueName, "sunpinyin") == 0) {
                    FCITX_DEF_MODULE_ARGS(args, lastpos, &isshuangpin);
                    result = FcitxSunPinyinInvokeGetFullPinyin(
                        cloudpinyin->owner, args);
                } else if (strcmp(im->uniqueName, "shuangpin") == 0) {
                    isshuangpin = true;
                    result = FcitxPinyinSP2QP(cloudpinyin->owner, lastpos);
                }
                if (isshuangpin) {
                    if (result) {
                        if (plength + strlen(result) < MAX_USER_INPUT) {
                            strcat(p + plength, result);
                            plength += strlen(result);
                            free(result);
                        } else {
                            p[hzlength] = '\0';
                            break;
                        }
                    }
                } else {
                    if (plength + strlen(lastpos) < MAX_USER_INPUT) {
                        strcat(p + plength, lastpos);
                        plength += strlen(lastpos);
                    } else {
                        p[hzlength] = '\0';
                        break;
                    }
                }
            }
            lastpos = pinyin + 1;
        }
        pinyin ++;
    } while(endflag);
    free(string);
    /* no pinyin append, return NULL for off it */
    if (p[hzlength] == '\0') {
        *ascii_part = NULL;
        return NULL;
    } else {
        char *res = strdup(p);
        *ascii_part = res + hzlength;
        return res;
    }
}

void SogouParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char* str = fcitx_utils_trim(queue->str);
    const char* ime_patch_key = "ime_patch_key = \"";
    size_t len = strlen(str);
    if (len == SOGOU_KEY_LENGTH + strlen(ime_patch_key) + 1
        && strncmp(str, ime_patch_key, strlen(ime_patch_key)) == 0
        && str[len - 1] == '\"') {
        sscanf(str,"ime_patch_key = \"%s\"", cloudpinyin->key);
        cloudpinyin->initialized = true;
        cloudpinyin->key[SOGOU_KEY_LENGTH] = '\0';
    }

    free(str);
}

void CloudPinyinHookForNewRequest(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    if (!cloudpinyin->initialized && !cloudpinyin->isrequestkey) {
        CloudPinyinRequestKey(cloudpinyin);
    }
}

// kate: indent-mode cstyle; space-indent on; indent-width 0;
