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
#import <AppKit/AppKit.h>
#include <SDL2/SDL.h>
#include "cocos/ui/edit-box/EditBox.h"
#elif CC_PLATFORM == CC_PLATFORM_IOS
#import <UIKit/UIKit.h>
#import <MobileCoreServices/MobileCoreServices.h>
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

static void completeFileSelection(const se::Value &callback, NSString *path, NSString *error = nil) {
    se::AutoHandleScope scope;
    callback.toObject()->call({path ? se::Value(path.UTF8String) : se::Value::Null,
                              error ? se::Value(error.UTF8String) : se::Value::Null}, nullptr);
}

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

static bool JSB_selectFile(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 2 && args[0].isString() && args[1].isObject() && args[1].toObject()->isFunction(), false, "Expected a file extension and callback");
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedFileTypes = @[[NSString stringWithUTF8String:args[0].toString().c_str()]];
    panel.allowsOtherFileTypes = NO;
    const bool selected = [panel runModal] == NSModalResponseOK;
    completeFileSelection(args[1], selected ? panel.URL.path : nil);
    return true;
}
SE_BIND_FUNC(JSB_selectFile)

#if CC_USE_EDITBOX
static bool JSB_setInputBoxText(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 3 && args[0].isString() && args[1].isNumber() && args[2].isNumber(), false, "Expected text and selection range");
    EditBox::setText(args[0].toString(), args[1].toInt32(), args[2].toInt32());
    return true;
}
SE_BIND_FUNC(JSB_setInputBoxText)

static bool JSB_getInputBoxSelection(se::State &s) {
    int start = 0;
    int end = 0;
    if (EditBox::getSelection(start, end)) {
        se::HandleObject range(se::Object::createPlainObject());
        range->setProperty("start", se::Value(start));
        range->setProperty("end", se::Value(end));
        s.rval().setObject(range);
    } else {
        s.rval().setNull();
    }
    return true;
}
SE_BIND_FUNC(JSB_getInputBoxSelection)
#endif
#endif

#if CC_PLATFORM == CC_PLATFORM_IOS
@interface CCDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@end

static CCDocumentPickerDelegate *filePickerDelegate = nil;
static UIDocumentPickerViewController *filePicker = nil;
static se::Value filePickerCallback;

static void clearFilePicker() {
    filePicker.delegate = nil;
    [filePicker release];
    filePicker = nil;
    filePickerCallback.setUndefined();
}

@implementation CCDocumentPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    se::Value callback = std::move(filePickerCallback);
    clearFilePicker();
    NSURL *url = urls.firstObject;
    if (url.isFileURL && [NSFileManager.defaultManager isReadableFileAtPath:url.path]) {
        completeFileSelection(callback, url.path);
    } else {
        completeFileSelection(callback, nil, @"Could not read the selected file");
    }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    se::Value callback = std::move(filePickerCallback);
    clearFilePicker();
    completeFileSelection(callback, nil);
}
@end

static bool JSB_selectFile(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 2 && args[0].isString() && args[1].isObject() && args[1].toObject()->isFunction(), false, "Expected a file extension and callback");
    if (filePicker) {
        completeFileSelection(args[1], nil, @"A file selection is already open");
        return true;
    }
    UIViewController *presenter = UIApplication.sharedApplication.keyWindow.rootViewController;
    while (presenter.presentedViewController) presenter = presenter.presentedViewController;
    if (!presenter.view.window || presenter.isBeingDismissed) {
        completeFileSelection(args[1], nil, @"File selection has no active window");
        return true;
    }
    NSString *extension = [NSString stringWithUTF8String:args[0].toString().c_str()];
    CFStringRef type = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, (CFStringRef)extension, nullptr);
    if (!type) {
        completeFileSelection(args[1], nil, @"Unknown file extension");
        return true;
    }
    // Import a local copy so cloud/provider files are readable without security-scoped access.
    filePicker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[(NSString *)type] inMode:UIDocumentPickerModeImport];
    CFRelease(type);
    if (!filePickerDelegate) filePickerDelegate = [CCDocumentPickerDelegate new];
    filePicker.delegate = filePickerDelegate;
    filePicker.allowsMultipleSelection = NO;
    filePickerCallback.setObject(args[1].toObject(), true);
    [presenter presentViewController:filePicker animated:YES completion:nil];
    return true;
}
SE_BIND_FUNC(JSB_selectFile)

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
    __jsbObj->defineFunction("selectFile", _SE(JSB_selectFile));
#if CC_USE_EDITBOX
    __jsbObj->defineFunction("setInputBoxText", _SE(JSB_setInputBoxText));
    __jsbObj->defineFunction("getInputBoxSelection", _SE(JSB_getInputBoxSelection));
#endif
#elif CC_PLATFORM == CC_PLATFORM_IOS
    __jsbObj->defineFunction("selectFile", _SE(JSB_selectFile));
    se::ScriptEngine::getInstance()->addBeforeCleanupHook([]() {
        [filePicker dismissViewControllerAnimated:NO completion:nil];
        clearFilePicker();
        [filePickerDelegate release];
        filePickerDelegate = nil;
    });
    __jsbObj->defineFunction("hideInputBoxAccessory", _SE(JSB_hideInputBoxAccessory));
#endif
    return true;
}
