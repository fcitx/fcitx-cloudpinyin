#include <fcitx/fcitx.h>
#include <fcitx/module.h>
#include <fcitx/instance.h>
#include <fcitx/hook.h>
#include <curl/curl.h>
#include "cloudpinyin.h"
#include <fcitx-utils/log.h>
#include <fcitx/candidate.h>

#define LOGLEVEL DEBUG

typedef struct _CloudCandWord {
    boolean filled;
} CloudCandWord;

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
static CloudPinyinCache* CloudPinyinAddToCache(FcitxCloudPinyin* cloudpinyin, const char* pinyin, const char* string);
static INPUT_RETURN_VALUE CloudPinyinGetCandWord(void* arg, CandidateWord* candWord);
static void _CloudPinyinAddCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin);
static void CloudPinyinFillCandidateWord(FcitxCloudPinyin* cloudpinyin, const char* pinyin);

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
    cloudpinyin->owner = instance;

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
        return;

    /* check whether the current im is pinyin */
    if (strcmp(im->strIconName, "pinyin") == 0 ||
            strcmp(im->strIconName, "googlepinyin") == 0 ||
            strcmp(im->strIconName, "sunpinyin") == 0)
    {
        /* there is something pending input */
        if (strlen(input->strCodeInput) >= 5)
        {
            CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, input->strCodeInput);
            FcitxLog(LOGLEVEL, "%s", input->strCodeInput);
            if (cacheEntry == NULL)
                CloudPinyinAddInputRequest(cloudpinyin, input->strCodeInput);
            _CloudPinyinAddCandidateWord(cloudpinyin, input->strCodeInput);
        }
    }
    
    return;
}

void CloudPinyinRequestKey(FcitxCloudPinyin* cloudpinyin)
{
    int still_running;
    CURL* curl = curl_easy_init();
    if (!curl)
        return;
    CurlQueue* queue = fcitx_malloc0(sizeof(CurlQueue)), *head = cloudpinyin->queue;
    queue->curl = curl;
    queue->next = NULL;
    
    while(head->next != NULL)
        head = head->next;
    head->next = queue;
    queue->type = RequestKey;
    
    curl_easy_setopt(curl, CURLOPT_URL, "http://web.pinyin.sogou.com/web_ime/patch.php");
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
    
    while(head->next != NULL)
        head = head->next;
    head->next = queue;
    queue->type = RequestPinyin;
    queue->pinyin = strdup(strPinyin);
    char *url = NULL;
    asprintf(&url, "http://web.pinyin.sogou.com/api/py?key=%s&query=%s", cloudpinyin->key, strPinyin);
    
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
        if (queue->http_code == 200)
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
        else
        {
            // CloudPinyinRequestKey(cloudpinyin);
        }
    }
    else if (queue->type == RequestPinyin)
    {
        if (queue->http_code == 200)
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
                    if (realstring)
                    {
                        CloudPinyinCache* cacheEntry = CloudPinyinCacheLookup(cloudpinyin, queue->pinyin);
                        if (cacheEntry == NULL)
                            cacheEntry = CloudPinyinAddToCache(cloudpinyin, queue->pinyin, realstring);
                        
                        FcitxIM* im = GetCurrentIM(cloudpinyin->owner);
                        if (strcmp(input->strCodeInput, queue->pinyin) == 0)
                        {
                            if (strcmp(im->strIconName, "pinyin") == 0 ||
                                strcmp(im->strIconName, "googlepinyin") == 0 ||
                                strcmp(im->strIconName, "sunpinyin") == 0)
                            {
                                CloudPinyinFillCandidateWord(cloudpinyin, input->strCodeInput);
                            }
                        }   
                    }

                }
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
    
    if((unsigned long long)((nmemb | size) &
        ((unsigned long long)SIZE_MAX << (sizeof(size_t) << 2))) &&
        (realsize / size != nmemb))
        return 0;

    if(SIZE_MAX - queue->size - 1 < realsize)
        realsize = SIZE_MAX - queue->size - 1;
    
    if(queue->str != NULL)
        queue->str = realloc(queue->str, queue->size + realsize + 1);
    else
        queue->str = fcitx_malloc0(realsize + 1);
    
    if(queue->str != NULL) {
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

CloudPinyinCache* CloudPinyinAddToCache(FcitxCloudPinyin* cloudpinyin, const char* pinyin, const char* string)
{
    CloudPinyinCache* cacheEntry = fcitx_malloc0(sizeof(CloudPinyinCache));
    cacheEntry->pinyin = strdup(pinyin);
    cacheEntry->str = strdup(string);
    HASH_ADD_KEYPTR(hh, cloudpinyin->cache, cacheEntry->pinyin, strlen(cacheEntry->pinyin), cacheEntry);
    
    /* if there is too much cached, remove the first one, though LRU might be a better algorithm */
    if (HASH_COUNT(cloudpinyin->cache) > MAX_CACHE_ENTRY)
    {
        HASH_DEL(cloudpinyin->cache, cloudpinyin->cache);
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
        candWord.strWord = strdup("☯");
    }

    candWord.callback = CloudPinyinGetCandWord;
    candWord.owner = cloudpinyin;
    candWord.priv = cloudCand;
    candWord.strExtra = strdup(_(" (via cloud)"));
    
    CandidateWordInsert(cloudpinyin->owner->input.candList, &candWord, 1);
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
        strcpy(GetOutputString(&cloudpinyin->owner->input), candWord->strWord);
        return IRV_COMMIT_STRING;
    }
    else
        return IRV_DO_NOTHING;
}