#ifndef FCITX_CLOUDPINYIN_H
#define FCITX_CLOUDPINYIN_H
#include <curl/curl.h>
#include <fcitx-config/fcitx-config.h>
#include <libintl.h>

#define SOGOU_KEY_LENGTH 32
#define MAX_CACHE_ENTRY 2048

#define _(x) gettext(x)

struct _FcitxInstance;

typedef enum _CloudPinyinRequestType 
{
    RequestKey,
    RequestPinyin
} CloudPinyinRequestType ;

typedef struct _CurlQueue
{
    CURL* curl;
    struct _CurlQueue* next;
    CloudPinyinRequestType type;
    int curl_result;
    int http_code;
    char* str;
    char* pinyin;
    size_t size;
} CurlQueue;

typedef struct _CloudPinyinCache
{
    char* pinyin;
    char* str;
    UT_hash_handle hh;
} CloudPinyinCache;

typedef struct _FcitxCloudPinyin
{    
    struct _FcitxInstance* owner;
    CURLM* curlm;
    CurlQueue* queue;
    char key[SOGOU_KEY_LENGTH + 1];
    boolean initialized;
    CloudPinyinCache* cache;
} FcitxCloudPinyin;

#endif