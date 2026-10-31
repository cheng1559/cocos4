/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.

 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#pragma once

#include <SDL2/SDL.h>
#include "cocos/base/Ptr.h"
#include "cocos/base/std/container/string.h"
#include "cocos/base/std/container/unordered_map.h"
#include "cocos/bindings/jswrapper/SeApi.h"
#include "cocos/platform/FileUtils.h"
#include "cocos/platform/Image.h"

namespace cc {

/** A colour cursor from an RGBA image file, such as a game's own arrow. Each path is decoded once and kept for the app's lifetime. */
inline SDL_Cursor *getSdlImageCursor(const ccstd::string &path, int hotX, int hotY) {
    static ccstd::unordered_map<ccstd::string, SDL_Cursor *> cursors;
    const auto cached = cursors.find(path);
    if (cached != cursors.end()) return cached->second;

    IntrusivePtr<Image> image = ccnew Image();
    if (!image->initWithImageFile(FileUtils::getInstance()->fullPathForFilename(path)) || image->getRenderFormat() != gfx::Format::RGBA8) {
        return nullptr;
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(image->getData(), image->getWidth(), image->getHeight(), 32,
                                                              image->getWidth() * 4, SDL_PIXELFORMAT_RGBA32);
    SDL_Cursor *cursor = surface ? SDL_CreateColorCursor(surface, hotX, hotY) : nullptr;
    SDL_FreeSurface(surface);
    if (cursor) cursors.emplace(path, cursor);
    return cursor;
}

/** jsb.setCursorImage(path, hotX, hotY): shows the image as the mouse cursor; false if it cannot be loaded. */
inline bool setSdlCursorImage(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 3 && args[0].isString() && args[1].isNumber() && args[2].isNumber(), false,
                     "Expected an image path and hotspot");
    SDL_Cursor *cursor = getSdlImageCursor(args[0].toString(), args[1].toInt32(), args[2].toInt32());
    if (cursor) SDL_SetCursor(cursor);
    s.rval().setBoolean(cursor != nullptr);
    return true;
}

} // namespace cc
