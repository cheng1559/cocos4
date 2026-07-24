/****************************************************************************
 Copyright (c) 2017-2022 Xiamen Yaji Software Co., Ltd.

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

#import "jsb_platform.h"

#include "cocos/bindings/jswrapper/SeApi.h"
#include "cocos/bindings/manual/jsb_conversions.h"
#include "cocos/bindings/manual/jsb_global.h"
#include "cocos/platform/FileUtils.h"
#include "cocos/platform/interfaces/modules/ISystemWindow.h"

#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#if CC_PLATFORM == CC_PLATFORM_MACOS
#include <SDL2/SDL.h>
#elif CC_PLATFORM == CC_PLATFORM_IOS
#import <UIKit/UIKit.h>
#endif
#include <regex>

using namespace cc;

static ccstd::unordered_map<ccstd::string, ccstd::string> _fontFamilyNameMap;
#if CC_PLATFORM == CC_PLATFORM_MACOS
static SDL_Cursor *systemCursor{nullptr};
#endif

const ccstd::unordered_map<ccstd::string, ccstd::string> &getFontFamilyNameMap() {
    return _fontFamilyNameMap;
}

static bool JSB_loadFont(se::State &s) {
    const auto &args = s.args();
    size_t argc = args.size();
    CC_UNUSED bool ok = true;
    if (argc >= 1) {
        s.rval().setNull();

        ccstd::string originalFamilyName;
        ok &= sevalue_to_native(args[0], &originalFamilyName);
        SE_PRECONDITION2(ok, false, "Error processing argument: originalFamilyName");

        // Don't reload font again to avoid memory leak.
        if (_fontFamilyNameMap.find(originalFamilyName) != _fontFamilyNameMap.end()) {
            s.rval().setString(_fontFamilyNameMap[originalFamilyName]);
            return true;
        }

        ccstd::string source;
        ok &= sevalue_to_native(args[1], &source);
        SE_PRECONDITION2(ok, false, "Error processing argument: source");

        ccstd::string fontFilePath;
        std::regex re("url\\(\\s*'\\s*(.*?)\\s*'\\s*\\)");
        std::match_results<ccstd::string::const_iterator> results;
        if (std::regex_search(source.cbegin(), source.cend(), results, re)) {
            fontFilePath = results[1].str();
        }

        fontFilePath = FileUtils::getInstance()->fullPathForFilename(fontFilePath);
        if (fontFilePath.empty()) {
            SE_LOGE("Font (%s) doesn't exist!", fontFilePath.c_str());
            return true;
        }

        NSData *dynamicFontData = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:fontFilePath.c_str()]];
        if (!dynamicFontData) {
            SE_LOGE("load font (%s) failed!", source.c_str());
            return true;
        }

        bool succeed = true;
        CFErrorRef error;
        CGDataProviderRef providerRef = CGDataProviderCreateWithCFData((CFDataRef)dynamicFontData);
        CGFontRef font = CGFontCreateWithDataProvider(providerRef);
        if (!CTFontManagerRegisterGraphicsFont(font, &error)) {
            CFStringRef errorDescription = CFErrorCopyDescription(error);
            const char *cErrorStr = CFStringGetCStringPtr(errorDescription, kCFStringEncodingUTF8);
            SE_LOGE("Failed to load font: %s", cErrorStr);
            CFRelease(errorDescription);
            succeed = false;
        }

        if (succeed) {
            CFStringRef fontName = CGFontCopyFullName(font);
            ccstd::string familyName([(NSString *)fontName UTF8String]);

            if (!familyName.empty()) {
                _fontFamilyNameMap.emplace(originalFamilyName, familyName);
                s.rval().setString(familyName);
            }
            CFRelease(fontName);
        }

        CFRelease(font);
        CFRelease(providerRef);
        return true;
    }

    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(JSB_loadFont)

#if CC_PLATFORM == CC_PLATFORM_MACOS
static SDL_Window *getMainWindow() {
    return SDL_GetWindowFromID(ISystemWindow::mainWindowId);
}

static bool JSB_isWindowFullScreen(se::State &s) {
    auto *window = getMainWindow();
    s.rval().setBoolean(window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
    return true;
}
SE_BIND_FUNC(JSB_isWindowFullScreen)

static bool JSB_setWindowFullScreen(se::State &s) {
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
SE_BIND_FUNC(JSB_setWindowFullScreen)

static bool JSB_setCursorStyle(se::State &s) {
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
SE_BIND_FUNC(JSB_setCursorStyle)
#endif

#if CC_PLATFORM == CC_PLATFORM_IOS
static bool JSB_hideInputBoxAccessory(se::State &s) {
    bool changed = false;
    for (UIWindow *window in UIApplication.sharedApplication.windows) {
        NSMutableArray<UIView *> *views = [NSMutableArray arrayWithObject:window];
        while (views.count > 0) {
            UIView *view = views.lastObject;
            [views removeLastObject];
            [views addObjectsFromArray:view.subviews];
            if ([view isKindOfClass:UITextField.class] || [view isKindOfClass:UITextView.class]) {
                id input = view;
                [input setInputAccessoryView:nil];
                if ([input respondsToSelector:@selector(inputAssistantItem)]) {
                    UITextInputAssistantItem *assistant = [input inputAssistantItem];
                    assistant.leadingBarButtonGroups = @[];
                    assistant.trailingBarButtonGroups = @[];
                }
                [input reloadInputViews];
                changed = true;
            }
        }
    }
    s.rval().setBoolean(changed);
    return true;
}
SE_BIND_FUNC(JSB_hideInputBoxAccessory)
#endif

bool register_platform_bindings(se::Object *obj) {
    __jsbObj->defineFunction("loadFont", _SE(JSB_loadFont));
#if CC_PLATFORM == CC_PLATFORM_MACOS
    __jsbObj->defineFunction("isWindowFullScreen", _SE(JSB_isWindowFullScreen));
    __jsbObj->defineFunction("setWindowFullScreen", _SE(JSB_setWindowFullScreen));
    __jsbObj->defineFunction("setCursorStyle", _SE(JSB_setCursorStyle));
#elif CC_PLATFORM == CC_PLATFORM_IOS
    __jsbObj->defineFunction("hideInputBoxAccessory", _SE(JSB_hideInputBoxAccessory));
#endif
    return true;
}
