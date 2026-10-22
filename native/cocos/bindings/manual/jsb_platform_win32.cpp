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
#include <Windows.h>
#include "cocos/platform/SDLHelper.h"

#include <regex>
#include <algorithm>
#include <cmath>

using namespace cc;

static ccstd::unordered_map<ccstd::string, ccstd::string> fontFamilyNameMap;
static SDL_Cursor *systemCursor{nullptr};
static HWND aspectWindow{nullptr};
static WNDPROC previousWindowProc{nullptr};
static double minimumWindowAspect{0};
static double maximumWindowAspect{0};

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

static void constrainWindowRect(HWND window, RECT &rect, bool vertical, UINT edge = 0) {
    RECT frame{}, client{};
    GetWindowRect(window, &frame);
    GetClientRect(window, &client);
    const LONG borderWidth = frame.right - frame.left - client.right;
    const LONG borderHeight = frame.bottom - frame.top - client.bottom;
    const double width = rect.right - rect.left - borderWidth, height = rect.bottom - rect.top - borderHeight;
    if (width <= 0 || height <= 0) return;
    if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) {
        vertical = std::abs(height - client.bottom) > std::abs(width - client.right);
    }
    if (vertical) {
        const LONG outerHeight = static_cast<LONG>(minimumWindowAspect == maximumWindowAspect
            ? std::round(width / minimumWindowAspect)
            : std::clamp(height, std::ceil(width / maximumWindowAspect), std::floor(width / minimumWindowAspect))) + borderHeight;
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) rect.top = rect.bottom - outerHeight;
        else rect.bottom = rect.top + outerHeight;
    } else {
        const LONG outerWidth = static_cast<LONG>(minimumWindowAspect == maximumWindowAspect
            ? std::round(height * minimumWindowAspect)
            : std::clamp(width, std::ceil(height * minimumWindowAspect), std::floor(height * maximumWindowAspect))) + borderWidth;
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) rect.left = rect.right - outerWidth;
        else rect.right = rect.left + outerWidth;
    }
}

static LRESULT CALLBACK aspectWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *sdlWindow = getMainWindow();
    if (sdlWindow && !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN) && !IsIconic(window)) {
        if (message == WM_SIZING) {
            constrainWindowRect(window, *reinterpret_cast<RECT *>(lParam), wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM,
                                static_cast<UINT>(wParam));
            return TRUE;
        }
        if (message == WM_WINDOWPOSCHANGING) {
            auto &position = *reinterpret_cast<WINDOWPOS *>(lParam);
            if (!(position.flags & SWP_NOSIZE)) {
                RECT frame{}, client{};
                GetWindowRect(window, &frame);
                GetClientRect(window, &client);
                const LONG width = position.cx - (frame.right - frame.left - client.right);
                const LONG height = position.cy - (frame.bottom - frame.top - client.bottom);
                RECT rect{0, 0, position.cx, position.cy};
                constrainWindowRect(window, rect, width < height * minimumWindowAspect);
                if (!(position.flags & SWP_NOMOVE)) {
                    position.x += (position.cx - rect.right) / 2;
                    position.y += (position.cy - rect.bottom) / 2;
                }
                position.cx = rect.right;
                position.cy = rect.bottom;
            }
        }
    }
    return CallWindowProcW(previousWindowProc, window, message, wParam, lParam);
}

static bool jsbSetWindowAspectRatio(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 2 && args[0].isNumber() && args[1].isNumber(), false, "Expected minimum and maximum window aspect ratios");
    const double minimum = args[0].toDouble(), maximum = args[1].toDouble();
    SE_PRECONDITION2(std::isfinite(minimum) && std::isfinite(maximum) && minimum > 0 && maximum >= minimum,
                     false, "Invalid window aspect ratio range");
    auto *sdlWindow = getMainWindow();
    if (!sdlWindow) { s.rval().setBoolean(false); return true; }
    const auto window = reinterpret_cast<HWND>(SDLHelper::getWindowHandle(sdlWindow));
    minimumWindowAspect = minimum;
    maximumWindowAspect = maximum;
    if (!aspectWindow) {
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(aspectWindowProc)));
        if (!previousWindowProc) { s.rval().setBoolean(false); return true; }
        aspectWindow = window;
    }
    RECT rect{};
    GetWindowRect(window, &rect);
    SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    s.rval().setBoolean(true);
    return true;
}
SE_BIND_FUNC(jsbSetWindowAspectRatio)

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
    __jsbObj->defineFunction("setWindowAspectRatio", _SE(jsbSetWindowAspectRatio));
    se::ScriptEngine::getInstance()->addBeforeCleanupHook([]() {
        if (aspectWindow && IsWindow(aspectWindow)) SetWindowLongPtrW(aspectWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousWindowProc));
        aspectWindow = nullptr;
        previousWindowProc = nullptr;
    });
    __jsbObj->defineFunction("isWindowFullScreen", _SE(jsbIsWindowFullScreen));
    __jsbObj->defineFunction("setWindowFullScreen", _SE(jsbSetWindowFullScreen));
    __jsbObj->defineFunction("setCursorStyle", _SE(jsbSetCursorStyle));
    return true;
}
