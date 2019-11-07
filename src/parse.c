
#include "config.h"

#include <string.h>
#include <fcitx-utils/utils.h>
#include <fcitx-utils/utf8.h>
#include <iconv.h>
#include <ctype.h>
#include "cloudpinyin.h"

char* GoogleParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char *start = NULL, *end = NULL;
    if (!queue->str) {
        return NULL;
    }
    if ((start = strstr(queue->str, "\",[\"")) != NULL)
    {
        start += strlen( "\",[\"");
        if ((end = strstr(start, "\"")) != NULL)
        {
            size_t length = end - start;
            char *realstring = fcitx_utils_malloc0(sizeof(char) * (length + 1));
            strncpy(realstring, start, length);
            realstring[length] = '\0';
            if (fcitx_utf8_check_string(realstring)) {
                return realstring;
            } else {
                free(realstring);
                return NULL;
            }
        }
    }
    return NULL;
}

char* BaiduParsePinyin(FcitxCloudPinyin* cloudpinyin, CurlQueue* queue)
{
    char *start = NULL, *end = NULL;
    if (!queue->str) {
        return NULL;
    }
    if ((start = strstr(queue->str, "[[\"")) != NULL)
    {
        start += strlen( "[[\"");
        if ((end = strstr(start, "\",")) != NULL)
        {
            size_t length = end - start;
            char *realstring = fcitx_utils_malloc0(sizeof(char) * (length + 1));
            strncpy(realstring, start, length);
            realstring[length] = '\0';
            if (fcitx_utf8_check_string(realstring)) {
                return realstring;
            } else {
                free(realstring);
                return NULL;
            }
        }
    }
    return NULL;
}
