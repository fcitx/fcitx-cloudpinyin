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

#include <errno.h>
#include <iconv.h>
#include <unistd.h>

#include <curl/curl.h>
#include <fcntl.h>

#include <fcitx/fcitx.h>
#include <fcitx/module.h>
#include <fcitx/instance.h>
#include <fcitx/hook.h>
#include <fcitx-utils/log.h>
#include <fcitx/candidate.h>
#include <fcitx-config/xdg.h>
#include <fcitx/module/pinyin/pydef.h>

#include "config.h"
#include "cloudpinyin.h"
#include "fetch.h"

#define CHECK_VALID_IM (im && \
                        (strcmp(im->uniqueName, "pinyin") == 0 || \
                        strcmp(im->uniqueName, "pinyin-libpinyin") == 0 || \
                        strcmp(im->uniqueName, "shuangpin-libpinyin") == 0 || \
                        strcmp(im->uniqueName, "googlepinyin") == 0 || \
                        strcmp(im->uniqueName, "sunpinyin") == 0 || \
                        strcmp(im->uniqueName, "shuangpin") == 0))
                        
#define CLOUDPINYIN_CHECK_PAGE_NUMBER 3

#define LOGLEVEL DEBUG

#ifdef LIBICONV_SECOND_ARGUMENT_IS_CONST
typedef const char* IconvStr;
#else
typedef char* IconvStr;
#endif

typedef struct _CloudCandWord {
    boolean filled;
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
static char *GetCurrentString(FcitxCloudPinyin* cloudpinyin);
static char* SplitHZAndPY(char* string);
static void CloudPinyinHookForNewRequest(void* arg);
static CURL* CloudPinyinGetFreeCurlHandle(FcitxCloudPinyin* cloudpinyin);
static void CloudPinyinReleaseCurlHandle(FcitxCloudPinyin* cloudpinyin, CURL* curl);

void SogouParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
char* SogouParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
void QQParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
char* QQParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
char* GoogleParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
char* BaiduParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);

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

FCITX_EXPORT_API
FcitxModule module = {
    CloudPinyinCreate,
    CloudPinyinSetFD,
    CloudPinyinProcessEvent,
    CloudPinyinDestroy,
    CloudPinyinReloadConfig
};

FCITX_EXPORT_API
int ABI_VERSION = FCITX_ABI_VERSION;

static inline boolean ishex(char ch)
{
    if ((ch >= '0' && ch <= '9') || (ch >='a' && ch <='f') || (ch >='A' && ch <='F'))
        return true;
    return false;
}

static inline unsigned char tohex(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >='a' && ch <='f')
        return ch - 'a' + 10;
    if (ch >='A' && ch <='F')
        return ch - 'A' + 10;
    return 0;
}

void* CloudPinyinCreate(FcitxInstance* instance)
{
    FcitxCloudPinyin* cloudpinyin = fcitx_utils_malloc0(sizeof(FcitxCloudPinyin));
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
            strToFree = GetCurrentString(cloudpinyin);
            inputString = SplitHZAndPY(strToFree);

            if (inputString)
            {
                CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, inputString);
                FcitxLog(LOGLEVEL, "%s", inputString);
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
    char c;
    while (read(cloudpinyin->pipeRecv, &c, sizeof(char)) > 0);
    pthread_mutex_lock(&cloudpinyin->finishQueueLock);
    CurlQueue* queue;
    queue = cloudpinyin->finishQueue;
    while (queue->next != NULL)
    {
        CurlQueue* pivot = queue->next;
        queue->next = queue->next->next;
        CloudPinyinHandleRequest(cloudpinyin, pivot);
    }
    pthread_mutex_unlock(&cloudpinyin->finishQueueLock);
}

void CloudPinyinDestroy(void* arg)
{

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
                strToFree = GetCurrentString(cloudpinyin);
                inputString = SplitHZAndPY(strToFree);

                if (inputString)
                {
                    FcitxLog(LOGLEVEL, "fill: %s %s", inputString, queue->pinyin);
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
    if (queue->str)
        free(queue->str);
    if (queue->pinyin)
        free(queue->pinyin);
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
    struct _FcitxCandidateWordList* candList = FcitxInputStateGetCandidateList(input);
    
    if (cacheEntry) {
        FcitxCandidateWord* cand;
        /* only check the first three page */
        int size = FcitxCandidateWordGetPageSize(candList) * CLOUDPINYIN_CHECK_PAGE_NUMBER;
        int i = 0;
        for (cand = FcitxCandidateWordGetFirst(FcitxInputStateGetCandidateList(input));
             cand != NULL;
             cand = FcitxCandidateWordGetNext(FcitxInputStateGetCandidateList(input), cand))
        {
            if (strcmp(cand->strWord, cacheEntry->str) == 0)
                return;
            i ++;
            if (i >= size)
                break;
        }
    }

    FcitxCandidateWord candWord;
    CloudCandWord* cloudCand = fcitx_utils_malloc0(sizeof(CloudCandWord));
    if (cacheEntry)
    {
        cloudCand->filled = true;
        candWord.strWord = strdup(cacheEntry->str);
    }
    else
    {
        cloudCand->filled = false;
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

    int order = cloudpinyin->config.iCandidateOrder - 1;
    if (order < 0)
        order = 0;

    FcitxCandidateWordInsert(candList, &candWord, order);
}

void CloudPinyinFillCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin)
{
    CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, pinyin);
    FcitxInputState* input = FcitxInstanceGetInputState(cloudpinyin->owner);
    struct _FcitxCandidateWordList* candList = FcitxInputStateGetCandidateList(input);
    if (cacheEntry)
    {
        FcitxCandidateWord* candWord;
        for (candWord = FcitxCandidateWordGetFirst(candList);
             candWord != NULL;
             candWord = FcitxCandidateWordGetNext(candList, candWord))
        {
            if (candWord->owner == cloudpinyin)
                break;
        }

        if (candWord == NULL)
            return;

        CloudCandWord* cloudCand = candWord->priv;
        if (cloudCand->filled)
            return;

        FcitxCandidateWord* cand;
        int i = 0;
        int size = FcitxCandidateWordGetPageSize(candList) * CLOUDPINYIN_CHECK_PAGE_NUMBER;
        for (cand = FcitxCandidateWordGetFirst(candList);
             cand != NULL;
             cand = FcitxCandidateWordGetNext(candList, cand))
        {
            if (strcmp(cand->strWord, cacheEntry->str) == 0) {
                FcitxCandidateWordRemove(candList, candWord);
                FcitxUIUpdateInputWindow(cloudpinyin->owner);
                candWord = NULL;
                break;
            }
            i ++;
            if (i >= size)
                break;
        }
        
        if (candWord)
        {
            if (cloudCand->filled == false)
            {
                cloudCand->filled = true;
                free(candWord->strWord);
                candWord->strWord = strdup(cacheEntry->str);
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
        char* string = GetCurrentString(cloudpinyin);
        char* py = SplitHZAndPY(string);
        if (py)
        {
            *py = 0;

            snprintf(FcitxInputStateGetOutputString(input), MAX_USER_INPUT, "%s%s", string, candWord->strWord);

            FcitxIM* im = FcitxInstanceGetCurrentIM(cloudpinyin->owner);
            FcitxModuleFunctionArg args;
            args.args[0] = FcitxInputStateGetOutputString(input);
            if (im)
            {
                if (strcmp(im->uniqueName, "sunpinyin") == 0)
                {
                    //InvokeModuleFunctionWithName(cloudpinyin->owner, "fcitx-sunpinyin", 1, args);
                }
                else if (strcmp(im->uniqueName, "shuangpin") == 0 || strcmp(im->uniqueName, "pinyin") == 0)
                {
                    FcitxModuleInvokeFunctionByName(cloudpinyin->owner, "fcitx-pinyin", 7, args);
                }
            }
        }
        if (string)
            free(string);
        return IRV_COMMIT_STRING;
    }
    else
        return IRV_DO_NOTHING;
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

char *GetCurrentString(FcitxCloudPinyin* cloudpinyin)
{
    FcitxIM* im = FcitxInstanceGetCurrentIM(cloudpinyin->owner);
    if (!im)
        return NULL;
    FcitxInputState* input = FcitxInstanceGetInputState(cloudpinyin->owner);
    char* string = FcitxUIMessagesToCString(FcitxInputStateGetPreedit(input));
    char p[MAX_USER_INPUT + 1], *pinyin, *lastpos;
    pinyin = SplitHZAndPY(string);
    lastpos = pinyin;
    boolean endflag;
    int hzlength = pinyin - string;
    size_t plength = hzlength;
    strncpy(p, string, hzlength);
    p[hzlength] = '\0';
    do
    {
        endflag = (*pinyin != '\0');

        if (*pinyin == ' ' || *pinyin == '\'' || *pinyin == '\0')
        {
            *pinyin = 0;

            if (*lastpos != '\0')
            {
                char* result = NULL;
                FcitxModuleFunctionArg arg;
                arg.args[0] = lastpos;
                boolean isshuangpin = false;
                if (strcmp(im->uniqueName, "sunpinyin") == 0)
                {
                    boolean issp = false;
                    arg.args[1] = &issp;
                    result = FcitxModuleInvokeFunctionByName(cloudpinyin->owner, "fcitx-sunpinyin", 0, arg);
                    isshuangpin = issp;
                }
                else if (strcmp(im->uniqueName, "shuangpin") == 0)
                {
                    isshuangpin = true;
                    result = InvokeFunction(cloudpinyin->owner, FCITX_PINYIN, SP2QP, arg);
                }
                if (isshuangpin)
                {
                    if (result)
                    {
                        if (plength + strlen(result) < MAX_USER_INPUT)
                        {
                            strcat(p + plength, result);
                            plength += strlen(result);
                            free(result);
                        }
                        else
                        {
                            p[hzlength] = '\0';
                            break;
                        }
                    }
                }
                else
                {
                    if (plength + strlen(lastpos) < MAX_USER_INPUT)
                    {
                        strcat(p + plength, lastpos);
                        plength += strlen(lastpos);
                    }
                    else
                    {
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
    if (p[hzlength] == '\0')
        return NULL;
    else
        return strdup(p);
}

char* SplitHZAndPY(char* string)
{
    if (string == NULL)
        return NULL;

    char* s = string;
    while (*s)
    {
        char* p;
        int chr;

        p = fcitx_utf8_get_char(s, &chr);
        if (p - s == 1)
            break;
        s = p;
    }

    return s;
}

void SogouParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char* str = fcitx_utils_trim(queue->str);
    const char* ime_patch_key = "ime_patch_key = \"";
    size_t len = strlen(str);
    if (len == SOGOU_KEY_LENGTH + strlen(ime_patch_key) + 1
            && strncmp(str, ime_patch_key, strlen(ime_patch_key)) == 0
            && str[len - 1] == '\"'
        )
    {
        sscanf(str,"ime_patch_key = \"%s\"", cloudpinyin->key);
        cloudpinyin->initialized = true;
        cloudpinyin->key[SOGOU_KEY_LENGTH] = '\0';
    }

    free(str);
}

char* SogouParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char *start = NULL, *end = NULL;
    if ((start = strchr(queue->str, '"')) != NULL && (end = strstr(queue->str, "%EF%BC%9A")) != NULL)
    {
        start ++;
        if (start < end)
        {
            size_t length = end - start;
            int conv_length;
            char *unescapedstring = curl_easy_unescape(queue->curl, start, length, &conv_length);
            char *realstring = strdup(unescapedstring);
            curl_free(unescapedstring);
            return realstring;
        }
    }
    return NULL;
}

void QQParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char* str = fcitx_utils_trim(queue->str);
    const char* ime_patch_key = "{\"key\":\"";
    if (strncmp(str, ime_patch_key, strlen(ime_patch_key)) == 0)
    {
        if (sscanf(str,"{\"key\":\"%32s\",\"ret\":\"suc\"}", cloudpinyin->key) > 0)
        {
            cloudpinyin->initialized = true;
            cloudpinyin->key[QQ_KEY_LENGTH] = '\0';
        }
    }

    free(str);
}

char* QQParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char *start = NULL, *end = NULL;
    if ((start = strstr(queue->str, "\"rs\":[\"")) != NULL)
    {
        start += strlen( "\"rs\":[\"");
        if ((end = strstr(start, "\"")) != NULL)
        {
            size_t length = end - start;
            char *realstring = fcitx_utils_malloc0(sizeof(char) * (length + 1));
            strncpy(realstring, start, length);
            realstring[length] = '\0';
            return realstring;
        }
    }
    return NULL;
}

char* GoogleParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char *start = NULL, *end = NULL;
    if ((start = strstr(queue->str, "\",[\"")) != NULL)
    {
        start += strlen( "\",[\"");
        if ((end = strstr(start, "\"")) != NULL)
        {
            size_t length = end - start;
            char *realstring = fcitx_utils_malloc0(sizeof(char) * (length + 1));
            strncpy(realstring, start, length);
            realstring[length] = '\0';
            return realstring;
        }
    }
    return NULL;
}

char* BaiduParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char *start = NULL, *end = NULL;
    static iconv_t conv = 0;
    if (conv == 0)
        conv = iconv_open("utf-8", "utf-16be");

    if (conv == (iconv_t)(-1))
        return NULL;
    if ((start = strstr(queue->str, "[[[\"")) != NULL)
    {
        start += strlen( "[[[\"");
        if ((end = strstr(start, "\",")) != NULL)
        {
            size_t length = end - start;
            if (length % 6 != 0 || length == 0)
                return NULL;

            size_t i = 0, j = 0;
            char* buf = fcitx_utils_malloc0((length / 6 + 1) * 2);
            while (i < length)
            {
                if (start[i] == '\\' && start[i+1] == 'u')
                {
                    if (ishex(start[i+2]) && ishex(start[i+3]) && ishex(start[i+4]) && ishex(start[i+5]))
                    {
                        buf[j++] = (tohex(start[i+2]) << 4) | tohex(start[i+3]);
                        buf[j++] = (tohex(start[i+4]) << 4) | tohex(start[i+5]);
                    }
                    else
                        break;
                }

                i += 6;
            }

            if (i != length)
            {
                free(buf);
                return NULL;
            }
            buf[j++] = 0;
            buf[j++] = 0;
            size_t len = UTF8_MAX_LENGTH * (length / 6) * sizeof(char);
            char* realstring = fcitx_utils_malloc0(UTF8_MAX_LENGTH * (length / 6) * sizeof(char));
            IconvStr p = buf; char *pp = realstring;
            iconv(conv, &p, &j, &pp, &len);

            free(buf);
            if (fcitx_utf8_check_string(realstring))
                return realstring;
            else
            {
                free(realstring);
                return NULL;
            }
        }
    }
    return NULL;
}



void CloudPinyinHookForNewRequest(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    if (!cloudpinyin->initialized && !cloudpinyin->isrequestkey)
    {
        CloudPinyinRequestKey(cloudpinyin);
    }
}

// kate: indent-mode cstyle; space-indent on; indent-width 0;
