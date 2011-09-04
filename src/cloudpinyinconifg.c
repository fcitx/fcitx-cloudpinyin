/***************************************************************************
 *   Copyright (C) 2010~2010 by CSSlayer                                   *
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

#include "cloudpinyin.h"

/* USE fcitx provided macro to bind config and variable */
CONFIG_BINDING_BEGIN(FcitxCloudPinyinConfig);
CONFIG_BINDING_REGISTER("CloudPinyin", "CandidateOrder", iCandidateOrder);
CONFIG_BINDING_REGISTER("CloudPinyin", "MinimumPinyinLength", iMinimumPinyinLength);
CONFIG_BINDING_REGISTER("CloudPinyin", "UseOriginPinyinOnly", bUsePinyinOnly);
CONFIG_BINDING_REGISTER("CloudPinyin", "DontShowSource", bDontShowSource);
CONFIG_BINDING_REGISTER("CloudPinyin", "Source", source);
CONFIG_BINDING_END();

// kate: indent-mode cstyle; space-indent on; indent-width 0;
