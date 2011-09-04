/***************************************************************************
 *   Copyright (C) 2011~2011 by CSSlayer                                   *
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
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <fcitx/fcitx.h>
#include <fcitx/module.h>
#include <fcitx/instance.h>
#include <fcitx/hook.h>
#include <curl/curl.h>
#include "cloudpinyin.h"
#include <fcitx-utils/log.h>
#include <fcitx/candidate.h>
#include <fcitx-config/xdg.h>
#include <errno.h>

#define KEY_PRESS_INTERVAL 50

#define LOGLEVEL DEBUG

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
static void CloudPinyinHandleReqest(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
static size_t CloudPinyinWriteFunction(char *ptr, size_t size, size_t nmemb, void *userdata);
static CloudPinyinCache* CloudPinyinCacheLookup(FcitxCloudPinyin* cloudpinyin, const char* pinyin);
static CloudPinyinCache* CloudPinyinAddToCache(FcitxCloudPinyin* cloudpinyin, const char* pinyin, char* string);
static INPUT_RETURN_VALUE CloudPinyinGetCandWord(void* arg, CandidateWord* candWord);
static void _CloudPinyinAddCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin);
static void CloudPinyinFillCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin);
static boolean LoadCloudPinyinConfig(FcitxCloudPinyinConfig* fs);
static void SaveCloudPinyinConfig(FcitxCloudPinyinConfig* fs);
static char *GetCurrentString(FcitxCloudPinyin* cloudpinyin);
static char* SplitHZAndPY(char* string);

void SogouParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
char* SogouParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
void QQParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
char* QQParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);
char* GoogleParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue);

CloudPinyinEngine engine[3] =
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
        "http://www.google.com/inputtools/request?ime=pinyin&text=%s",
        NULL,
        GoogleParsePinyin
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

void* CloudPinyinCreate(FcitxInstance* instance)
{
    FcitxCloudPinyin* cloudpinyin = fcitx_malloc0(sizeof(FcitxCloudPinyin));
    bindtextdomain("fcitx-cloudpinyin", LOCALEDIR);
    cloudpinyin->owner = instance;

    if (!LoadCloudPinyinConfig(&cloudpinyin->config))
    {
        free(cloudpinyin);
        return NULL;
    }

    cloudpinyin->curlm = curl_multi_init();
    if (cloudpinyin->curlm == NULL)
    {
        free(cloudpinyin);
        return NULL;
    }

    cloudpinyin->queue = fcitx_malloc0(sizeof(CurlQueue));

    FcitxIMEventHook hook;
    hook.arg = cloudpinyin;
    hook.func = CloudPinyinAddCandidateWord;

    RegisterUpdateCandidateWordHook(instance, hook);

    CloudPinyinRequestKey(cloudpinyin);

    return cloudpinyin;
}

void CloudPinyinAddCandidateWord(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    FcitxIM* im = GetCurrentIM(cloudpinyin->owner);
    FcitxInputState* input = &cloudpinyin->owner->input;

    if (cloudpinyin->initialized == false)
    {
        if (!cloudpinyin->isrequestkey)
        {
            cloudpinyin->counter ++;
            if (cloudpinyin->counter >= KEY_PRESS_INTERVAL)
                CloudPinyinRequestKey(cloudpinyin);
        }
        return;
    }

    /* check whether the current im is pinyin */
    if (strcmp(im->strIconName, "pinyin") == 0 ||
            strcmp(im->strIconName, "googlepinyin") == 0 ||
            strcmp(im->strIconName, "sunpinyin") == 0)
    {
        /* there is something pending input */
        if (strlen(input->strCodeInput) >= cloudpinyin->config.iMinimumPinyinLength)
        {
            char* strToFree = NULL, *inputString;
            if (cloudpinyin->config.bUsePinyinOnly)
                inputString = input->strCodeInput;
            else
            {
                strToFree = GetCurrentString(cloudpinyin);
                inputString = SplitHZAndPY(strToFree);
            }
            CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, inputString);
            FcitxLog(LOGLEVEL, "%s", inputString);
            if (cacheEntry == NULL)
                CloudPinyinAddInputRequest(cloudpinyin, inputString);
            _CloudPinyinAddCandidateWord(cloudpinyin, inputString);

            if (strToFree)
                free(strToFree);
        }
    }

    return;
}

void CloudPinyinRequestKey(FcitxCloudPinyin* cloudpinyin)
{
    int still_running;
    if (cloudpinyin->isrequestkey)
        return;

    cloudpinyin->isrequestkey = true;
    if (engine[cloudpinyin->config.source].RequestKey == NULL)
    {
        cloudpinyin->initialized = true;
        cloudpinyin->key[0] = '\0';
        cloudpinyin->counter = 0;
        cloudpinyin->isrequestkey = false;
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl)
        return;
    CurlQueue* queue = fcitx_malloc0(sizeof(CurlQueue)), *head = cloudpinyin->queue;
    queue->curl = curl;
    queue->next = NULL;

    while (head->next != NULL)
        head = head->next;
    head->next = queue;
    queue->type = RequestKey;
    queue->source = cloudpinyin->config.source;

    curl_easy_setopt(curl, CURLOPT_URL, engine[cloudpinyin->config.source].RequestKey);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, queue);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CloudPinyinWriteFunction);
    curl_multi_add_handle(cloudpinyin->curlm, curl);
    CURLMcode mcode;
    do {
        mcode = curl_multi_perform(cloudpinyin->curlm, &still_running);
    } while (mcode == CURLM_CALL_MULTI_PERFORM);

    if (mcode != CURLM_OK)
    {
        FcitxLog(ERROR, "curl error");
    }
}



void CloudPinyinSetFD(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    FcitxInstance* instance = cloudpinyin->owner;
    int maxfd = 0;
    curl_multi_fdset(cloudpinyin->curlm, &instance->rfds, &instance->wfds, &instance->efds, &maxfd);
    if (maxfd > instance->maxfd)
        instance->maxfd = maxfd;
}

void CloudPinyinProcessEvent(void* arg)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    CURLMcode mcode;
    int still_running;
    do {
        mcode = curl_multi_perform(cloudpinyin->curlm, &still_running);
    } while (mcode == CURLM_CALL_MULTI_PERFORM);

    int num_messages = 0;
    CURLMsg* curl_message = curl_multi_info_read(cloudpinyin->curlm, &num_messages);;
    CurlQueue* queue, *previous;

    while (curl_message != NULL) {
        if (curl_message->msg == CURLMSG_DONE) {
            int curl_result = curl_message->data.result;
            previous = cloudpinyin->queue;
            queue = cloudpinyin->queue->next;
            while (queue != NULL &&
                    queue->curl != curl_message->easy_handle)
            {
                previous = queue;
                queue = queue->next;
            }
            if (queue != NULL) {
                curl_multi_remove_handle(cloudpinyin->curlm, queue->curl);
                previous->next = queue->next;
                queue->curl_result = curl_result;
                curl_easy_getinfo(queue->curl, CURLINFO_HTTP_CODE, &queue->http_code);
                CloudPinyinHandleReqest(cloudpinyin, queue);
            }
        } else {
            FcitxLog(ERROR, "Unknown CURL message received: %d\n",
                     (int)curl_message->msg);
        }
        curl_message = curl_multi_info_read(cloudpinyin->curlm, &num_messages);
    }
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
        cloudpinyin->counter = KEY_PRESS_INTERVAL;
    }
}

void CloudPinyinAddInputRequest(FcitxCloudPinyin* cloudpinyin, const char* strPinyin)
{
    int still_running;
    CURL* curl = curl_easy_init();
    if (!curl)
        return;
    CurlQueue* queue = fcitx_malloc0(sizeof(CurlQueue)), *head = cloudpinyin->queue;
    queue->curl = curl;
    queue->next = NULL;

    while (head->next != NULL)
        head = head->next;
    head->next = queue;
    queue->type = RequestPinyin;
    queue->pinyin = strdup(strPinyin);
    queue->source = cloudpinyin->config.source;
    char* urlstring = curl_escape(strPinyin, strlen(strPinyin));
    char *url = NULL;
    if (engine[cloudpinyin->config.source].RequestKey)
        asprintf(&url, engine[cloudpinyin->config.source].RequestPinyin, cloudpinyin->key, urlstring);
    else
        asprintf(&url, engine[cloudpinyin->config.source].RequestPinyin, urlstring);
    free(urlstring);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, queue);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CloudPinyinWriteFunction);
    curl_multi_add_handle(cloudpinyin->curlm, curl);

    free(url);
    CURLMcode mcode;
    do {
        mcode = curl_multi_perform(cloudpinyin->curlm, &still_running);
    } while (mcode == CURLM_CALL_MULTI_PERFORM);

    if (mcode != CURLM_OK)
    {
        FcitxLog(ERROR, "curl error");
    }
}

void CloudPinyinHandleReqest(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    FcitxInputState* input = &cloudpinyin->owner->input;
    if (queue->type == RequestKey)
    {
        cloudpinyin->isrequestkey = false;
        cloudpinyin->counter = 0;

        if (queue->source != cloudpinyin->config.source)
            return;

        if (queue->http_code == 200)
        {
            if (engine[cloudpinyin->config.source].ParseKey)
                engine[cloudpinyin->config.source].ParseKey(cloudpinyin, queue);
        }
        else
        {
            // CloudPinyinRequestKey(cloudpinyin);
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

                FcitxIM* im = GetCurrentIM(cloudpinyin->owner);

                char* strToFree = NULL, *inputString;
                if (cloudpinyin->config.bUsePinyinOnly)
                    inputString = input->strCodeInput;
                else
                {
                    strToFree = GetCurrentString(cloudpinyin);
                    inputString = SplitHZAndPY(strToFree);
                }
                FcitxLog(LOGLEVEL, "fill: %s %s", inputString, queue->pinyin);
                if (strcmp(inputString, queue->pinyin) == 0)
                {
                    if (strcmp(im->strIconName, "pinyin") == 0 ||
                            strcmp(im->strIconName, "googlepinyin") == 0 ||
                            strcmp(im->strIconName, "sunpinyin") == 0)
                    {
                        CloudPinyinFillCandidateWord(cloudpinyin, inputString);
                    }
                }
                if (strToFree)
                    free(strToFree);
                free(realstring);
            }
        }
    }
    curl_easy_cleanup(queue->curl);
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
        queue->str = fcitx_malloc0(realsize + 1);

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
    CloudPinyinCache* cacheEntry = fcitx_malloc0(sizeof(CloudPinyinCache));
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

    CandidateWord candWord;
    CloudCandWord* cloudCand = fcitx_malloc0(sizeof(CloudCandWord));
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
    if (cloudpinyin->config.bDontShowSource)
        candWord.strExtra = NULL;
    else
        candWord.strExtra = strdup(_(" (via cloud)"));

    int order = cloudpinyin->config.iCandidateOrder - 1;
    if (order < 0)
        order = 0;

    CandidateWordInsert(cloudpinyin->owner->input.candList, &candWord, order);
}

void CloudPinyinFillCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin)
{
    CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, pinyin);
    FcitxInputState* input = &cloudpinyin->owner->input;
    if (cacheEntry)
    {
        CandidateWord* candWord;
        for (candWord = CandidateWordGetFirst(input->candList);
                candWord != NULL;
                candWord = CandidateWordGetNext(input->candList, candWord))
        {
            if (candWord->owner == cloudpinyin)
                break;
        }

        if (candWord)
        {
            CloudCandWord* cloudCand = candWord->priv;
            if (cloudCand->filled == false)
            {
                cloudCand->filled = true;
                free(candWord->strWord);
                candWord->strWord = strdup(cacheEntry->str);
                UpdateInputWindow(cloudpinyin->owner);
            }
        }
    }
}

INPUT_RETURN_VALUE CloudPinyinGetCandWord(void* arg, CandidateWord* candWord)
{
    FcitxCloudPinyin* cloudpinyin = (FcitxCloudPinyin*) arg;
    CloudCandWord* cloudCand = candWord->priv;
    if (cloudCand->filled)
    {
        if (cloudpinyin->config.bUsePinyinOnly)
        {
            strcpy(GetOutputString(&cloudpinyin->owner->input), candWord->strWord);
        }
        else
        {
            char* string = GetCurrentString(cloudpinyin);
            char* py = SplitHZAndPY(string);
            *py = 0;

            strncpy(GetOutputString(&cloudpinyin->owner->input), string, MAX_USER_INPUT);
            strncat(GetOutputString(&cloudpinyin->owner->input), candWord->strWord, MAX_USER_INPUT);

            free(string);
        }
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
    ConfigFileDesc *configDesc = GetCloudPinyinConfigDesc();
    if (configDesc == NULL)
        return false;

    FILE *fp = GetXDGFileUserWithPrefix("conf", "fcitx-cloudpinyin.config", "rt", NULL);

    if (!fp)
    {
        if (errno == ENOENT)
            SaveCloudPinyinConfig(fs);
    }
    ConfigFile *cfile = ParseConfigFileFp(fp, configDesc);
    FcitxCloudPinyinConfigConfigBind(fs, cfile, configDesc);
    ConfigBindSync(&fs->config);

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
    ConfigFileDesc *configDesc = GetCloudPinyinConfigDesc();
    FILE *fp = GetXDGFileUserWithPrefix("conf", "fcitx-cloudpinyin.config", "wt", NULL);
    SaveConfigFileFp(fp, &fs->config, configDesc);
    if (fp)
        fclose(fp);
}

char *GetCurrentString(FcitxCloudPinyin* cloudpinyin)
{
    char* string = MessagesToCString(cloudpinyin->owner->input.msgPreedit);
    char* p = string, *pp = string;
    while(*p)
    {
        if (*p != ' ')
        {
            *pp = *p;
            pp ++;
        }
        p ++;
    }
    *pp = 0;
    return string;
}

char* SplitHZAndPY(char* string)
{
    char* s = string;
    while (*s)
    {
        char* p;
        int chr;

        p = utf8_get_char(s, &chr);
        if (p - s == 1)
            break;
        s = p;
    }

    return s;
}

void SogouParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char* str = fcitx_trim(queue->str);
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
            char *realstring = curl_easy_unescape(queue->curl, start, length, &conv_length);
            return realstring;
        }
    }
    return NULL;
}

void QQParseKey(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char* str = fcitx_trim(queue->str);
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
            char *realstring = fcitx_malloc0(sizeof(char) * (length + 1));
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
            char *realstring = fcitx_malloc0(sizeof(char) * (length + 1));
            strncpy(realstring, start, length);
            realstring[length] = '\0';
            return realstring;
        }
    }
    return NULL;
}


// kate: indent-mode cstyle; space-indent on; indent-width 0;
