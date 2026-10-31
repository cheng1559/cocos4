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
#include "cocos/ui/edit-box/EditBox.h"

#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#if CC_PLATFORM == CC_PLATFORM_MACOS
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <SDL2/SDL.h>
#include "cocos/platform/SDLHelper.h"
#include "cocos/bindings/manual/jsb_sdl_cursor.h"
#elif CC_PLATFORM == CC_PLATFORM_IOS
#import <UIKit/UIKit.h>
#import <MobileCoreServices/MobileCoreServices.h>
#endif
#include <regex>
#include <algorithm>
#include <cmath>

using namespace cc;

static ccstd::unordered_map<ccstd::string, ccstd::string> _fontFamilyNameMap;
#if CC_PLATFORM == CC_PLATFORM_MACOS
static SDL_Cursor *systemCursor{nullptr};

// Keep SDL's delegate responsible for input, close, focus and fullscreen events.
@interface CCWindowAspectDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, retain) id<NSWindowDelegate> originalDelegate;
@property(nonatomic, assign) SDL_Window *sdlWindow;
@property(nonatomic, assign) double minimumAspect;
@property(nonatomic, assign) double maximumAspect;
@end

@implementation CCWindowAspectDelegate
- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [self.originalDelegate respondsToSelector:selector];
}
- (id)forwardingTargetForSelector:(SEL)selector {
    return self.originalDelegate;
}
- (NSSize)constrainSize:(NSSize)size forWindow:(NSWindow *)window vertically:(BOOL)vertical {
    if ((window.styleMask & NSWindowStyleMaskFullScreen) ||
        (SDL_GetWindowFlags(self.sdlWindow) & SDL_WINDOW_FULLSCREEN)) return size;
    NSSize content = [window contentRectForFrameRect:NSMakeRect(0, 0, size.width, size.height)].size;
    if (content.width <= 0 || content.height <= 0) return size;
    if (vertical) {
        content.height = self.minimumAspect == self.maximumAspect
            ? std::round(content.width / self.minimumAspect)
            : std::clamp(content.height, std::ceil(content.width / self.maximumAspect),
                         std::floor(content.width / self.minimumAspect));
    } else {
        content.width = self.minimumAspect == self.maximumAspect
            ? std::round(content.height * self.minimumAspect)
            : std::clamp(content.width, std::ceil(content.height * self.minimumAspect),
                         std::floor(content.height * self.maximumAspect));
    }
    return [window frameRectForContentRect:NSMakeRect(0, 0, content.width, content.height)].size;
}
- (NSSize)windowWillResize:(NSWindow *)window toSize:(NSSize)size {
    if ([self.originalDelegate respondsToSelector:_cmd]) {
        size = [self.originalDelegate windowWillResize:window toSize:size];
    }
    const NSSize content = [window contentRectForFrameRect:NSMakeRect(0, 0, size.width, size.height)].size;
    const NSSize current = window.contentView.frame.size;
    return [self constrainSize:size forWindow:window
                   vertically:std::abs(content.height - current.height) > std::abs(content.width - current.width)];
}
- (void)windowDidResize:(NSNotification *)notification {
    NSWindow *window = notification.object;
    NSRect frame = window.frame;
    const NSSize content = window.contentView.frame.size;
    // Programmatic resize/zoom stays inside the requested frame, including portrait displays.
    const NSSize size = [self constrainSize:frame.size forWindow:window
                               vertically:content.width < content.height * self.minimumAspect];
    if (!NSEqualSizes(frame.size, size)) {
        frame.origin.y += frame.size.height - size.height;
        frame.size = size;
        [window setFrame:frame display:YES];
        return;
    }
    if ([self.originalDelegate respondsToSelector:_cmd]) [self.originalDelegate windowDidResize:notification];
}
- (void)dealloc {
    [_originalDelegate release];
    [super dealloc];
}
@end

static char windowAspectDelegateKey;
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

static void completeFileSelection(const se::Value &callback, NSArray<NSURL *> *urls, NSString *error = nil) {
    se::AutoHandleScope scope;
    se::HandleObject paths(se::Object::createArrayObject(urls.count));
    for (uint32_t i = 0; i < urls.count; ++i) paths->setArrayElement(i, se::Value(urls[i].path.UTF8String));
    callback.toObject()->call({se::Value(paths),
                              error ? se::Value(error.UTF8String) : se::Value::Null}, nullptr);
}

#if CC_PLATFORM == CC_PLATFORM_MACOS
static SDL_Window *getMainWindow() {
    return SDL_GetWindowFromID(ISystemWindow::mainWindowId);
}

static bool JSB_setWindowAspectRatio(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 2 && args[0].isNumber() && args[1].isNumber(), false, "Expected minimum and maximum window aspect ratios");
    const double minimum = args[0].toDouble(), maximum = args[1].toDouble();
    SE_PRECONDITION2(std::isfinite(minimum) && std::isfinite(maximum) && minimum > 0 && maximum >= minimum,
                     false, "Invalid window aspect ratio range");
    auto *sdlWindow = getMainWindow();
    if (!sdlWindow) { s.rval().setBoolean(false); return true; }
    auto *window = reinterpret_cast<NSWindow *>(SDLHelper::getWindowHandle(sdlWindow));
    auto *delegate = (CCWindowAspectDelegate *)objc_getAssociatedObject(window, &windowAspectDelegateKey);
    if (!delegate) {
        delegate = [[CCWindowAspectDelegate alloc] init];
        delegate.originalDelegate = window.delegate;
        delegate.sdlWindow = sdlWindow;
        objc_setAssociatedObject(window, &windowAspectDelegateKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [delegate release];
    }
    delegate.minimumAspect = minimum;
    delegate.maximumAspect = maximum;
    window.delegate = delegate;
    [delegate windowDidResize:[NSNotification notificationWithName:NSWindowDidResizeNotification object:window]];
    s.rval().setBoolean(true);
    return true;
}
SE_BIND_FUNC(JSB_setWindowAspectRatio)

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
SE_BIND_FUNC(setSdlCursorImage)

static bool JSB_selectFiles(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 2 && args[0].isString() && args[1].isObject() && args[1].toObject()->isFunction(), false, "Expected a file extension and callback");
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = YES;
    panel.allowedFileTypes = @[[NSString stringWithUTF8String:args[0].toString().c_str()]];
    panel.allowsOtherFileTypes = NO;
    const bool selected = [panel runModal] == NSModalResponseOK;
    completeFileSelection(args[1], selected ? panel.URLs : nil);
    return true;
}
SE_BIND_FUNC(JSB_selectFiles)
#endif

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
    completeFileSelection(callback, urls);
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    se::Value callback = std::move(filePickerCallback);
    clearFilePicker();
    completeFileSelection(callback, nil);
}
@end

static bool JSB_selectFiles(se::State &s) {
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
    filePicker.allowsMultipleSelection = YES;
    filePickerCallback.setObject(args[1].toObject(), true);
    [presenter presentViewController:filePicker animated:YES completion:nil];
    return true;
}
SE_BIND_FUNC(JSB_selectFiles)

#if CC_USE_EDITBOX
static bool JSB_hideInputBoxAccessory(se::State &s) {
    s.rval().setBoolean(EditBox::hideAccessory());
    return true;
}
SE_BIND_FUNC(JSB_hideInputBoxAccessory)
#endif
#endif

bool register_platform_bindings(se::Object *obj) {
    __jsbObj->defineFunction("loadFont", _SE(JSB_loadFont));
#if CC_USE_EDITBOX
    __jsbObj->defineFunction("setInputBoxText", _SE(JSB_setInputBoxText));
    __jsbObj->defineFunction("getInputBoxSelection", _SE(JSB_getInputBoxSelection));
#endif
#if CC_PLATFORM == CC_PLATFORM_MACOS
    __jsbObj->defineFunction("setWindowAspectRatio", _SE(JSB_setWindowAspectRatio));
    se::ScriptEngine::getInstance()->addBeforeCleanupHook([]() {
        auto *sdlWindow = getMainWindow();
        if (!sdlWindow) return;
        auto *window = reinterpret_cast<NSWindow *>(SDLHelper::getWindowHandle(sdlWindow));
        auto *delegate = (CCWindowAspectDelegate *)objc_getAssociatedObject(window, &windowAspectDelegateKey);
        if (delegate && window.delegate == delegate) window.delegate = delegate.originalDelegate;
        objc_setAssociatedObject(window, &windowAspectDelegateKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    });
    __jsbObj->defineFunction("isWindowFullScreen", _SE(JSB_isWindowFullScreen));
    __jsbObj->defineFunction("setWindowFullScreen", _SE(JSB_setWindowFullScreen));
    __jsbObj->defineFunction("setCursorStyle", _SE(JSB_setCursorStyle));
    __jsbObj->defineFunction("setCursorImage", _SE(setSdlCursorImage));
    __jsbObj->defineFunction("selectFiles", _SE(JSB_selectFiles));
#if CC_USE_EDITBOX
    __jsbObj->setProperty("onInputBoxKey", se::Value::Null);
#endif
#elif CC_PLATFORM == CC_PLATFORM_IOS
    __jsbObj->defineFunction("selectFiles", _SE(JSB_selectFiles));
    se::ScriptEngine::getInstance()->addBeforeCleanupHook([]() {
        [filePicker dismissViewControllerAnimated:NO completion:nil];
        clearFilePicker();
        [filePickerDelegate release];
        filePickerDelegate = nil;
    });
#if CC_USE_EDITBOX
    __jsbObj->defineFunction("hideInputBoxAccessory", _SE(JSB_hideInputBoxAccessory));
#endif
#endif
    return true;
}
