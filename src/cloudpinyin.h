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

#ifndef FCITX_CLOUDPINYIN_H
#define FCITX_CLOUDPINYIN_H
#include <curl/curl.h>
#include <fcitx-config/fcitx-config.h>
#include <libintl.h>

#define SOGOU_KEY_LENGTH 32
#define QQ_KEY_LENGTH 32
#define MAX_CACHE_ENTRY 2048
#define MAX_ERROR 10

#define _(x) dgettext("fcitx-cloudpinyin", (x))

struct _FcitxInstance;

typedef enum _CloudPinyinSource
{
    CloudPinyin_Sogou = 0,
    CloudPinyin_QQ = 1,
    CloudPinyin_Google = 2
} CloudPinyinSource;

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
    CloudPinyinSource source;
} CurlQueue;

typedef struct _CloudPinyinCache
{
    char* pinyin;
    char* str;
    UT_hash_handle hh;
} CloudPinyinCache;

typedef struct _FcitxCloudPinyinConfig
{
    GenericConfig config;
    int iCandidateOrder;
    int iMinimumPinyinLength;
    boolean bDontShowSource;
    CloudPinyinSource source;
} FcitxCloudPinyinConfig;

typedef struct _FcitxCloudPinyin
{
    struct _FcitxInstance* owner;
    FcitxCloudPinyinConfig config;
    CURLM* curlm;
    CurlQueue* queue;
    int errorcount;
    char key[SOGOU_KEY_LENGTH + 1];
    boolean initialized;
    CloudPinyinCache* cache;
    boolean isrequestkey;
} FcitxCloudPinyin;

CONFIG_BINDING_DECLARE(FcitxCloudPinyinConfig);

#endif
// kate: indent-mode cstyle; space-indent on; indent-width 0;
