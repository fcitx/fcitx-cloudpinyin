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

#ifndef FCITX_CLOUDPINYIN_H
#define FCITX_CLOUDPINYIN_H
#include <curl/curl.h>
#include <fcitx-config/fcitx-config.h>
#include <fcitx/instance.h>
#include <libintl.h>

#define SOGOU_KEY_LENGTH 32
#define QQ_KEY_LENGTH 32
#define MAX_CACHE_ENTRY 2048
#define MAX_ERROR 10
#define MAX_HANDLE 10l

#define _(x) dgettext("fcitx-cloudpinyin", (x))

typedef enum {
    CloudPinyin_Sogou = 0,
    CloudPinyin_QQ = 1,
    CloudPinyin_Google = 2,
    CloudPinyin_Baidu = 3
} CloudPinyinSource;

typedef enum {
    RequestKey,
    RequestPinyin
} CloudPinyinRequestType ;

typedef struct {
    boolean used;
    CURL* curl;
} CurlFreeListItem;

typedef struct _CurlQueue {
    CURL* curl;
    struct _CurlQueue* next;
    CloudPinyinRequestType type;
    int curl_result;
    long http_code;
    char* str;
    char* pinyin;
    size_t size;
    CloudPinyinSource source;
} CurlQueue;

typedef struct {
    char* pinyin;
    char* str;
    UT_hash_handle hh;
} CloudPinyinCache;

typedef struct {
    FcitxGenericConfig config;
    int iCandidateOrder;
    int iMinimumPinyinLength;
    boolean bDontShowSource;
    CloudPinyinSource source;
    FcitxHotkeys hkToggle;
    boolean bEnabled;
} FcitxCloudPinyinConfig;

typedef struct {
    FcitxInstance* owner;
    FcitxCloudPinyinConfig config;
    CurlQueue* pendingQueue;
    CurlQueue* finishQueue;

    pthread_mutex_t pendingQueueLock;
    pthread_mutex_t finishQueueLock;

    int pipeNotify;
    int pipeRecv;
    int errorcount;
    char key[SOGOU_KEY_LENGTH + 1];
    boolean initialized;
    CloudPinyinCache* cache;
    boolean isrequestkey;
    struct _FcitxFetchThread* fetch;

    CurlFreeListItem freeList[MAX_HANDLE];

    pthread_t pid;
} FcitxCloudPinyin;

CONFIG_BINDING_DECLARE(FcitxCloudPinyinConfig);

#endif
// kate: indent-mode cstyle; space-indent on; indent-width 0;
