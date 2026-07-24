/****************************************************************************
 Copyright (c) 2017-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "jsb_platform.h"

#include "cocos/bindings/jswrapper/SeApi.h"
#include "cocos/bindings/manual/jsb_conversions.h"
#include "cocos/bindings/manual/jsb_global_init.h"
#include "cocos/platform/FileUtils.h"
#include "cocos/platform/interfaces/modules/ISystemWindow.h"

#include <SDL2/SDL.h>

#include <regex>

using namespace cc;

static ccstd::unordered_map<ccstd::string, ccstd::string> fontFamilyNameMap;
static SDL_Cursor *systemCursor{nullptr};

const ccstd::unordered_map<ccstd::string, ccstd::string> &getFontFamilyNameMap() {
    return fontFamilyNameMap;
}

static bool jsbLoadFont(se::State &s) {
    const auto &args = s.args();
    size_t argc = args.size();
    CC_UNUSED bool ok = true;
    if (argc >= 1) {
        s.rval().setNull();

        ccstd::string originalFamilyName;
        ok &= sevalue_to_native(args[0], &originalFamilyName);
        SE_PRECONDITION2(ok, false, "Error processing argument: originalFamilyName");

        ccstd::string source;
        ok &= sevalue_to_native(args[1], &source);
        SE_PRECONDITION2(ok, false, "Error processing argument: source");

        ccstd::string fontFilePath;
        std::regex re(R"(url\(\s*'\s*(.*?)\s*'\s*\))");
        std::match_results<ccstd::string::const_iterator> results;
        if (std::regex_search(source.cbegin(), source.cend(), results, re)) {
            fontFilePath = results[1].str();
        }

        fontFilePath = FileUtils::getInstance()->fullPathForFilename(fontFilePath);
        if (fontFilePath.empty()) {
            SE_LOGE("Font (%s) doesn't exist!", fontFilePath.c_str());
            return true;
        }

        // just put the path info, used at CanvasRenderingContext2DImpl::updateFont()
        fontFamilyNameMap.emplace(originalFamilyName, fontFilePath);

        s.rval().setString(originalFamilyName);

        return true;
    }

    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(jsbLoadFont)

static SDL_Window *getMainWindow() {
    return SDL_GetWindowFromID(ISystemWindow::mainWindowId);
}

static bool jsbIsWindowFullScreen(se::State &s) {
    auto *window = getMainWindow();
    s.rval().setBoolean(window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
    return true;
}
SE_BIND_FUNC(jsbIsWindowFullScreen)

static bool jsbSetWindowFullScreen(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 1 && args[0].isBoolean(), false, "Expected a fullscreen boolean");

    auto *window = getMainWindow();
    const bool success = window && SDL_SetWindowFullscreen(
        window, args[0].toBoolean() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0;
    if (success && !args[0].toBoolean()) {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    s.rval().setBoolean(success);
    return true;
}
SE_BIND_FUNC(jsbSetWindowFullScreen)

static bool jsbSetCursorStyle(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 1 && args[0].isString(), false, "Expected a cursor style string");

    SDL_SystemCursor cursorId = SDL_SYSTEM_CURSOR_ARROW;
    const auto &style = args[0].toString();
    if (style == "pointer") {
        cursorId = SDL_SYSTEM_CURSOR_HAND;
    } else if (style == "move") {
        cursorId = SDL_SYSTEM_CURSOR_SIZEALL;
    } else if (style == "text") {
        cursorId = SDL_SYSTEM_CURSOR_IBEAM;
    }

    auto *cursor = SDL_CreateSystemCursor(cursorId);
    if (cursor) {
        SDL_SetCursor(cursor);
        SDL_FreeCursor(systemCursor);
        systemCursor = cursor;
    }
    s.rval().setBoolean(cursor != nullptr);
    return true;
}
SE_BIND_FUNC(jsbSetCursorStyle)

bool register_platform_bindings(se::Object * /*obj*/) { // NOLINT(readability-identifier-naming)
    __jsbObj->defineFunction("loadFont", _SE(jsbLoadFont));
    __jsbObj->defineFunction("isWindowFullScreen", _SE(jsbIsWindowFullScreen));
    __jsbObj->defineFunction("setWindowFullScreen", _SE(jsbSetWindowFullScreen));
    __jsbObj->defineFunction("setCursorStyle", _SE(jsbSetCursorStyle));
    return true;
}
