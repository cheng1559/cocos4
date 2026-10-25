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
#include "cocos/platform/win32/Utils-win32.h"
#include "cocos/platform/win32/WindowsPlatform.h"
#include "cocos/ui/edit-box/EditBox.h"

#include <SDL2/SDL.h>
#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
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
static constexpr UINT_PTR RESIZE_TIMER_ID = 0xCC51;
static bool resizingWindow{false};
static RECT resizeStartClient{};
static SIZE resizeTargetSize{};

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
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        // Vertical drags may grow the width, but shortening the height must not shrink it.
        vertical = width > height * maximumWindowAspect;
    } else if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) {
        // Use a stable baseline so constraining one axis cannot flip the next event's drag direction.
        const auto &start = resizingWindow ? resizeStartClient : client;
        vertical = std::abs(width - start.right) > std::abs(height - start.bottom);
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

static void constrainWindowPosition(HWND window, WINDOWPOS &position) {
    MONITORINFO monitor{sizeof(MONITORINFO)};
    const bool hasWorkArea = GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
    RECT frame{}, client{};
    GetWindowRect(window, &frame);
    GetClientRect(window, &client);
    RECT rect{0, 0, position.cx, position.cy};
    const bool verticalStretch = hasWorkArea && position.cy >= monitor.rcWork.bottom - monitor.rcWork.top &&
        position.cx < monitor.rcWork.right - monitor.rcWork.left;
    if (hasWorkArea) {
        rect.right = std::min(rect.right, monitor.rcWork.right - monitor.rcWork.left);
        rect.bottom = std::min(rect.bottom, monitor.rcWork.bottom - monitor.rcWork.top);
    }
    const LONG width = rect.right - (frame.right - frame.left - client.right);
    const LONG height = rect.bottom - (frame.bottom - frame.top - client.bottom);
    constrainWindowRect(window, rect, verticalStretch ? width > height * maximumWindowAspect : width < height * minimumWindowAspect);
    if (hasWorkArea && rect.right > monitor.rcWork.right - monitor.rcWork.left) {
        rect.right = monitor.rcWork.right - monitor.rcWork.left;
        constrainWindowRect(window, rect, true);
    }
    if (!(position.flags & SWP_NOMOVE)) {
        position.x += (position.cx - rect.right) / 2;
        position.y += (position.cy - rect.bottom) / 2;
        if (hasWorkArea) {
            position.x = std::clamp<LONG>(position.x, monitor.rcWork.left, monitor.rcWork.right - rect.right);
            position.y = std::clamp<LONG>(position.y, monitor.rcWork.top, monitor.rcWork.bottom - rect.bottom);
        }
    }
    position.cx = rect.right;
    position.cy = rect.bottom;
}

static LRESULT CALLBACK aspectWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ENTERSIZEMOVE) {
        GetClientRect(window, &resizeStartClient);
        resizeTargetSize = {};
        resizingWindow = true;
        SetTimer(window, RESIZE_TIMER_ID, 16, nullptr);
    } else if (message == WM_EXITSIZEMOVE) {
        resizingWindow = false;
        KillTimer(window, RESIZE_TIMER_ID);
    }
    else if (message == WM_TIMER && wParam == RESIZE_TIMER_ID) {
        static_cast<WindowsPlatform *>(BasePlatform::getPlatform())->renderDuringResize();
        return 0;
    }
    auto *sdlWindow = getMainWindow();
    if (sdlWindow && !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN) && !IsIconic(window)) {
        if (message == WM_SIZING) {
            auto &rect = *reinterpret_cast<RECT *>(lParam);
            constrainWindowRect(window, rect, false, static_cast<UINT>(wParam));
            resizeTargetSize = {rect.right - rect.left, rect.bottom - rect.top};
            return TRUE;
        }
        if (message == WM_WINDOWPOSCHANGING) {
            auto &position = *reinterpret_cast<WINDOWPOS *>(lParam);
            // Skip only the WM_SIZING result; Windows Snap can submit a different size during the same drag.
            const bool alreadyConstrained = resizingWindow && position.cx == resizeTargetSize.cx && position.cy == resizeTargetSize.cy;
            if (!(position.flags & SWP_NOSIZE) && !alreadyConstrained) {
                constrainWindowPosition(window, position);
            }
        }
    }
    const auto result = CallWindowProcW(previousWindowProc, window, message, wParam, lParam);
    if (message == WM_GETMINMAXINFO && sdlWindow && !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN)) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            WINDOWPOS position{};
            position.x = monitor.rcWork.left;
            position.y = monitor.rcWork.top;
            position.cx = monitor.rcWork.right - monitor.rcWork.left;
            position.cy = monitor.rcWork.bottom - monitor.rcWork.top;
            constrainWindowPosition(window, position);
            auto &limits = *reinterpret_cast<MINMAXINFO *>(lParam);
            limits.ptMaxSize = {position.cx, position.cy};
            limits.ptMaxPosition = {position.x - monitor.rcMonitor.left, position.y - monitor.rcMonitor.top};
            return 0;
        }
    }
    // SDL must queue the new size before rendering; WM_TIMER alone can leave a stale frame during live resize.
    if (resizingWindow && (message == WM_SIZE || message == WM_PAINT)) {
        static_cast<WindowsPlatform *>(BasePlatform::getPlatform())->renderDuringResize();
    }
    return result;
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
    POINT cursor{};
    const bool restoreCursor = window && SDL_GetMouseFocus() == window &&
        SDL_GetRelativeMouseMode() == SDL_FALSE && GetCursorPos(&cursor);
    const bool success = window && SDL_SetWindowFullscreen(
        window, args[0].toBoolean() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0;
    if (success && !args[0].toBoolean()) {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    // SDL restores client coordinates during fullscreen changes; preserve the desktop position instead.
    if (restoreCursor) SetCursorPos(cursor.x, cursor.y);
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

static bool jsbSelectFiles(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 2 && args[0].isString() && args[1].isObject() && args[1].toObject()->isFunction(), false, "Expected a file extension and callback");
    SE_PRECONDITION2(std::regex_match(args[0].toString(), std::regex("[A-Za-z0-9]+")), false, "Invalid file extension");
    se::AutoHandleScope scope;
    se::HandleObject paths(se::Object::createArrayObject(0));
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT result = initialized;
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (SUCCEEDED(result)) result = CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()));
        DWORD options = 0;
        if (SUCCEEDED(result)) result = dialog->GetOptions(&options);
        if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_STRICTFILETYPES);
        const auto pattern = StringUtf8ToWideChar("*." + args[0].toString());
        const COMDLG_FILTERSPEC filter{pattern.c_str(), pattern.c_str()};
        if (SUCCEEDED(result)) result = dialog->SetFileTypes(1, &filter);
        auto *window = getMainWindow();
        if (SUCCEEDED(result)) result = dialog->Show(window ? reinterpret_cast<HWND>(SDLHelper::getWindowHandle(window)) : nullptr);
        Microsoft::WRL::ComPtr<IShellItemArray> selected;
        if (SUCCEEDED(result)) result = dialog->GetResults(selected.GetAddressOf());
        DWORD count = 0;
        if (SUCCEEDED(result)) result = selected->GetCount(&count);
        for (DWORD i = 0; SUCCEEDED(result) && i < count; ++i) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            result = selected->GetItemAt(i, item.GetAddressOf());
            PWSTR path = nullptr;
            if (SUCCEEDED(result)) result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(result)) paths->setArrayElement(i, se::Value(StringWideCharToUtf8(path)));
            CoTaskMemFree(path);
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    const auto error = FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED)
        ? se::Value("File selection failed (HRESULT " + std::to_string(static_cast<uint32_t>(result)) + ")") : se::Value::Null;
    args[1].toObject()->call({se::Value(paths), error}, nullptr);
    return true;
}
SE_BIND_FUNC(jsbSelectFiles)

#if CC_USE_EDITBOX
static bool jsbSetInputBoxText(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 3 && args[0].isString() && args[1].isNumber() && args[2].isNumber(), false, "Expected text and selection range");
    EditBox::setText(args[0].toString(), args[1].toInt32(), args[2].toInt32());
    return true;
}
SE_BIND_FUNC(jsbSetInputBoxText)

static bool jsbGetInputBoxSelection(se::State &s) {
    int start = 0, end = 0;
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
SE_BIND_FUNC(jsbGetInputBoxSelection)
#endif

bool register_platform_bindings(se::Object * /*obj*/) { // NOLINT(readability-identifier-naming)
#if CC_USE_EDITBOX
    __jsbObj->defineFunction("setInputBoxText", _SE(jsbSetInputBoxText));
    __jsbObj->defineFunction("getInputBoxSelection", _SE(jsbGetInputBoxSelection));
    __jsbObj->setProperty("onInputBoxKey", se::Value::Null);
#endif
    __jsbObj->defineFunction("loadFont", _SE(jsbLoadFont));
    __jsbObj->defineFunction("setWindowAspectRatio", _SE(jsbSetWindowAspectRatio));
    se::ScriptEngine::getInstance()->addBeforeCleanupHook([]() {
        if (aspectWindow && IsWindow(aspectWindow)) {
            KillTimer(aspectWindow, RESIZE_TIMER_ID);
            SetWindowLongPtrW(aspectWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousWindowProc));
        }
        aspectWindow = nullptr;
        previousWindowProc = nullptr;
        resizingWindow = false;
    });
    __jsbObj->defineFunction("isWindowFullScreen", _SE(jsbIsWindowFullScreen));
    __jsbObj->defineFunction("setWindowFullScreen", _SE(jsbSetWindowFullScreen));
    __jsbObj->defineFunction("setCursorStyle", _SE(jsbSetCursorStyle));
    __jsbObj->defineFunction("selectFiles", _SE(jsbSelectFiles));
    return true;
}
