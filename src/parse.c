
#include "config.h"

#include <string.h>
#include <fcitx-utils/utils.h>
#include <fcitx-utils/utf8.h>
#include <iconv.h>
#include <ctype.h>
#include "cloudpinyin.h"

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

char* MapSogouStringToHalf(const char* string)
{
    const char* s = string;
    const char* sn;
    size_t len = strlen(string);
    char* half = fcitx_utils_malloc0(sizeof(char) * (len + 1));
    char* halfp = half;
    int upperCount = 0;

    while (*s) {
        unsigned int chr = 0;

        sn = fcitx_utf8_get_char(s, &chr);

        /* from A to Z */
        if ((chr >= 0xff21 && chr <= 0xff3a) || (chr >= 0xff41 && chr <= 0xff5a)) {
            *halfp = (char) (chr & 0xff) + 0x20;
            if (isupper(*halfp))
                upperCount ++;
            halfp ++;
        }
        else {
            while(s < sn) {
                *halfp = *s;
                if (isupper(*halfp))
                    upperCount ++;
                s++;
                halfp++;
            }
        }

        s = sn;
    }
    if (*half && isupper(*half) && upperCount == 1) {
        *half = tolower(*half);
    }
    return half;
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
            char *realstring = MapSogouStringToHalf(unescapedstring);
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
    if ((start = strstr(queue->str, "[[\"")) != NULL)
    {
        start += strlen( "[[\"");
        if ((end = strstr(start, "\",")) != NULL)
        {
            size_t length = end - start;
            char *realstring = fcitx_utils_malloc0(sizeof(char) * (length + 1));
            strncpy(realstring, start, length);
            realstring[length] = '\0';
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
