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
#include <dwmapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include "cocos/platform/SDLHelper.h"
#include "cocos/bindings/manual/jsb_sdl_cursor.h"

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
// The client height the window keeps, in logical pixels; 0 lets it take any height.
static double lockedWindowHeight{0};
// The window has yet to be brought to that height (it was fullscreen): until then the height is not held.
static bool heightFitPending{false};
static constexpr UINT FIT_HEIGHT_MESSAGE = WM_APP + 0xC51;
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

/** The client height the window keeps, in its own pixels; 0 when it may take any. */
static LONG lockedClientHeight(HWND window) {
    if (lockedWindowHeight <= 0 || heightFitPending) return 0;
    return static_cast<LONG>(std::lround(lockedWindowHeight * GetDpiForWindow(window) / 96.0));
}

static void constrainWindowRect(HWND window, RECT &rect, bool vertical, UINT edge = 0) {
    RECT frame{}, client{};
    GetWindowRect(window, &frame);
    GetClientRect(window, &client);
    const LONG borderWidth = frame.right - frame.left - client.right;
    const LONG borderHeight = frame.bottom - frame.top - client.bottom;
    const double width = rect.right - rect.left - borderWidth, height = rect.bottom - rect.top - borderHeight;
    if (width <= 0 || height <= 0) return;
    if (const LONG lockedHeight = lockedClientHeight(window)) {
        // The height stays; the width follows the aspect range at that height.
        const LONG lockedWidth = static_cast<LONG>(minimumWindowAspect == maximumWindowAspect
            ? std::round(lockedHeight * minimumWindowAspect)
            : std::clamp(width, std::ceil(lockedHeight * minimumWindowAspect), std::floor(lockedHeight * maximumWindowAspect)));
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) rect.top = rect.bottom - lockedHeight - borderHeight;
        else rect.bottom = rect.top + lockedHeight + borderHeight;
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) rect.left = rect.right - lockedWidth - borderWidth;
        else rect.right = rect.left + lockedWidth + borderWidth;
        return;
    }
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

static void constrainWindowPosition(HWND window, WINDOWPOS &position, bool maximized = false) {
    maximized = maximized || IsZoomed(window);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    const bool hasWorkArea = GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
    RECT frame{}, client{};
    GetWindowRect(window, &frame);
    GetClientRect(window, &client);
    // Only a window asked for a new size is kept on the work area: one dragged by its caption keeps its size and, like
    // any other, may go past the screen's edges or onto another monitor.
    const bool moving = !maximized && position.cx == frame.right - frame.left && position.cy == frame.bottom - frame.top;
    const bool verticalStretch = !maximized && hasWorkArea && position.cy >= monitor.rcWork.bottom - monitor.rcWork.top &&
        position.cx < monitor.rcWork.right - monitor.rcWork.left;
    if (hasWorkArea) {
        RECT invisible{};
        if (maximized) {
            WINDOWINFO info{sizeof(WINDOWINFO)};
            if (GetWindowInfo(window, &info)) {
                invisible.left = invisible.right = static_cast<LONG>(info.cxWindowBorders);
                invisible.top = invisible.bottom = static_cast<LONG>(info.cyWindowBorders);
            }
        } else {
            RECT visible{};
            if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)))) {
                invisible = {std::max<LONG>(0, visible.left - frame.left), std::max<LONG>(0, visible.top - frame.top),
                             std::max<LONG>(0, frame.right - visible.right), std::max<LONG>(0, frame.bottom - visible.bottom)};
            }
        }
        // The work area bounds the visible frame, not the invisible resize border returned by GetWindowRect.
        monitor.rcWork.left -= invisible.left;
        monitor.rcWork.top -= invisible.top;
        monitor.rcWork.right += invisible.right;
        monitor.rcWork.bottom += invisible.bottom;
    }
    RECT rect{0, 0, position.cx, position.cy};
    if (hasWorkArea) {
        rect.right = maximized ? monitor.rcWork.right - monitor.rcWork.left
                               : std::min(rect.right, monitor.rcWork.right - monitor.rcWork.left);
        rect.bottom = maximized ? monitor.rcWork.bottom - monitor.rcWork.top
                                : std::min(rect.bottom, monitor.rcWork.bottom - monitor.rcWork.top);
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
        if (hasWorkArea && !moving) {
            position.x = std::clamp<LONG>(position.x, monitor.rcWork.left, monitor.rcWork.right - rect.right);
            position.y = std::clamp<LONG>(position.y, monitor.rcWork.top, monitor.rcWork.bottom - rect.bottom);
        }
    }
    position.cx = rect.right;
    position.cy = rect.bottom;
}

/** Brings the window to its locked height, at its aspect within the range, keeping its centre where it was. */
static void fitLockedHeight(HWND window) {
    RECT frame{}, client{};
    GetWindowRect(window, &frame);
    GetClientRect(window, &client);
    heightFitPending = false;
    const LONG lockedHeight = lockedClientHeight(window);
    if (!lockedHeight || client.right <= 0 || client.bottom <= 0) return;
    const double aspect = std::clamp(static_cast<double>(client.right) / client.bottom, minimumWindowAspect, maximumWindowAspect);
    const LONG width = static_cast<LONG>(std::lround(lockedHeight * aspect)) + frame.right - frame.left - client.right;
    const LONG height = lockedHeight + frame.bottom - frame.top - client.bottom;
    SetWindowPos(window, nullptr, (frame.left + frame.right - width) / 2, (frame.top + frame.bottom - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// True while SDL moves the window to the rect Windows suggests for a new DPI.
static bool changingDpi = false;


static LRESULT CALLBACK aspectWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_STYLECHANGING && wParam == GWL_STYLE && lockedWindowHeight > 0) {
        // Held at a height, the window is never maximized, whatever style SDL rebuilds (leaving fullscreen, say).
        reinterpret_cast<STYLESTRUCT *>(lParam)->styleNew &= ~WS_MAXIMIZEBOX;
    }
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
    else if (message == WM_GETDPISCALEDSIZE) {
        // Dragged to a monitor with another scale, keep the window's apparent size like other per-monitor aware
        // apps: FALSE asks Windows for linear scaling, where SDL would keep the client size in pixels.
        return FALSE;
    }
    auto *sdlWindow = getMainWindow();
    if (message == FIT_HEIGHT_MESSAGE) {
        if (heightFitPending && sdlWindow && !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN) && !IsIconic(window) &&
            !IsZoomed(window)) {
            fitLockedHeight(window);
        }
        return 0;
    }
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
            // Windows' suggested rect for a new DPI keeps the window under the cursor mid-drag; constraining it here
            // would use the old DPI's frame, re-centre it and pull it onto one monitor. The pass after it fixes the size.
            if (!(position.flags & SWP_NOSIZE) && !alreadyConstrained && !changingDpi) {
                constrainWindowPosition(window, position);
            }
        }
    }
    changingDpi = message == WM_DPICHANGED;
    const auto result = CallWindowProcW(previousWindowProc, window, message, wParam, lParam);
    changingDpi = false;
    if (message == WM_NCHITTEST && lockedClientHeight(window) > 0) {
        // Held at a height, only the side edges resize: the top edge moves the window like its caption, the bottom
        // edge is plain border, and a corner resizes the width alone.
        switch (result) {
            case HTTOP: return HTCAPTION;
            case HTBOTTOM: return HTBORDER;
            case HTTOPLEFT: case HTBOTTOMLEFT: return HTLEFT;
            case HTTOPRIGHT: case HTBOTTOMRIGHT: return HTRIGHT;
            default: break;
        }
    }
    if (message == WM_GETMINMAXINFO && sdlWindow && !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN)) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            WINDOWPOS position{};
            position.x = monitor.rcWork.left;
            position.y = monitor.rcWork.top;
            position.cx = monitor.rcWork.right - monitor.rcWork.left;
            position.cy = monitor.rcWork.bottom - monitor.rcWork.top;
            constrainWindowPosition(window, position, true);
            auto &limits = *reinterpret_cast<MINMAXINFO *>(lParam);
            limits.ptMaxSize = {position.cx, position.cy};
            limits.ptMaxPosition = {position.x - monitor.rcMonitor.left, position.y - monitor.rcMonitor.top};
            if (const LONG lockedHeight = lockedClientHeight(window)) {
                // SDL pins a window it may not resize at whatever size it has, and lets only its own resizes past
                // that; the locked range is the one that holds, for fitting the window to it and for DPI changes.
                RECT frame{}, client{};
                GetWindowRect(window, &frame);
                GetClientRect(window, &client);
                const LONG borderWidth = frame.right - frame.left - client.right;
                const LONG borderHeight = frame.bottom - frame.top - client.bottom;
                const bool single = minimumWindowAspect == maximumWindowAspect;
                const auto minimumWidth = static_cast<LONG>(single ? std::round(lockedHeight * minimumWindowAspect)
                                                                   : std::ceil(lockedHeight * minimumWindowAspect));
                const auto maximumWidth = static_cast<LONG>(single ? std::round(lockedHeight * maximumWindowAspect)
                                                                   : std::floor(lockedHeight * maximumWindowAspect));
                limits.ptMinTrackSize = {minimumWidth + borderWidth, lockedHeight + borderHeight};
                limits.ptMaxTrackSize = {maximumWidth + borderWidth, lockedHeight + borderHeight};
            }
            return 0;
        }
    }
    if (message == WM_DPICHANGED && sdlWindow && !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN) &&
        !IsIconic(window) && !IsZoomed(window)) {
        // Constrain the suggested size with the new DPI's frame, keeping the position.
        RECT rect{};
        GetWindowRect(window, &rect);
        SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // SDL must queue the new size before rendering; WM_TIMER alone can leave a stale frame during live resize.
    if (resizingWindow && (message == WM_SIZE || message == WM_PAINT)) {
        static_cast<WindowsPlatform *>(BasePlatform::getPlatform())->renderDuringResize();
    }
    // Back from fullscreen, once SDL has restored the window's own size.
    if (heightFitPending && message == WM_SIZE && wParam == SIZE_RESTORED && sdlWindow &&
        !(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN)) {
        PostMessageW(window, FIT_HEIGHT_MESSAGE, 0, 0);
    }
    return result;
}

/**
 * setWindowAspectRatio(minimum, maximum, height = 0): keeps the client area's aspect within the range. A height (logical
 * pixels) also holds the client area at that height: the window is scaled to it at its aspect, and only its width can
 * change after; with a single aspect it cannot be resized at all.
 */
static bool jsbSetWindowAspectRatio(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2((args.size() == 2 || args.size() == 3) && args[0].isNumber() && args[1].isNumber() &&
                     (args.size() == 2 || args[2].isNumber()), false, "Expected minimum and maximum window aspect ratios, and a height");
    const double minimum = args[0].toDouble(), maximum = args[1].toDouble();
    const double height = args.size() == 3 ? args[2].toDouble() : 0;
    SE_PRECONDITION2(std::isfinite(minimum) && std::isfinite(maximum) && minimum > 0 && maximum >= minimum,
                     false, "Invalid window aspect ratio range");
    SE_PRECONDITION2(std::isfinite(height) && height >= 0, false, "Invalid window height");
    auto *sdlWindow = getMainWindow();
    if (!sdlWindow) { s.rval().setBoolean(false); return true; }
    const auto window = reinterpret_cast<HWND>(SDLHelper::getWindowHandle(sdlWindow));
    minimumWindowAspect = minimum;
    maximumWindowAspect = maximum;
    lockedWindowHeight = height;
    // The height is held only once the window has been brought to it, so its aspect is still its own until then.
    heightFitPending = height > 0;
    if (!aspectWindow) {
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(aspectWindowProc)));
        if (!previousWindowProc) { s.rval().setBoolean(false); return true; }
        aspectWindow = window;
    }
    SDL_SetWindowResizable(sdlWindow, height > 0 && minimum == maximum ? SDL_FALSE : SDL_TRUE);
    // Held at a height, a window whose width may still change is not maximized either (WM_STYLECHANGING keeps it so);
    // freed, a resizable window gets its maximize button back. SDL leaves the style alone when resizability is unchanged.
    if (!(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN)) {
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR maximizable = height <= 0 && (SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_RESIZABLE) ? WS_MAXIMIZEBOX : 0;
        if ((style & WS_MAXIMIZEBOX) != maximizable) SetWindowLongPtrW(window, GWL_STYLE, (style & ~WS_MAXIMIZEBOX) | maximizable);
    }
    // SDL only changes the style: the frame and its maximize button are redrawn, and the frame's new thickness is taken
    // up, once Windows is told the frame changed. The size below then counts the new frame.
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (height > 0) {
        // A fullscreen or minimized window is brought to it when it is back (FIT_HEIGHT_MESSAGE).
        if (!(SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN) && !IsIconic(window)) {
            if (IsZoomed(window)) ShowWindow(window, SW_RESTORE);
            fitLockedHeight(window);
        }
    } else {
        RECT rect{};
        GetWindowRect(window, &rect);
        SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
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
    // Leaving fullscreen centres the window on the display it was fullscreen on, not the primary one.
    const int display = window ? std::max(0, SDL_GetWindowDisplayIndex(window)) : 0;
    const bool success = window && SDL_SetWindowFullscreen(
        window, args[0].toBoolean() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0;
    if (success && !args[0].toBoolean()) {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
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

    const auto &style = args[0].toString();
    // "none" hides the cursor over the window, e.g. while the game draws its own; any other style shows it again.
    if (style == "none") {
        s.rval().setBoolean(SDL_ShowCursor(SDL_DISABLE) >= 0);
        return true;
    }
    SDL_ShowCursor(SDL_ENABLE);
    SDL_SystemCursor cursorId = SDL_SYSTEM_CURSOR_ARROW;
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
SE_BIND_FUNC(setSdlCursorImage)

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
    __jsbObj->defineFunction("setCursorImage", _SE(setSdlCursorImage));
    __jsbObj->defineFunction("selectFiles", _SE(jsbSelectFiles));
    return true;
}
