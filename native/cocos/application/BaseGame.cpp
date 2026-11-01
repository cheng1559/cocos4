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

#include "BaseGame.h"
#include <cmath>
#include <string>
#include "ApplicationManager.h"
#include "platform/interfaces/modules/ISystemWindowManager.h"
#include "renderer/pipeline/GlobalDescriptorSetManager.h"

#if CC_PLATFORM == CC_PLATFORM_ANDROID
    #include "platform/android/adpf_manager.h"
#elif CC_PLATFORM == CC_PLATFORM_WINDOWS
    #include "SDL2/SDL.h"
#endif
extern "C" void cc_load_all_plugins(); // NOLINT

namespace cc {

BaseGame::~BaseGame() { // NOLINT
#if (CC_PLATFORM == CC_PLATFORM_ANDROID) && CC_SUPPORT_ADPF
    ADPFManager::getInstance().destroy();
#endif
}

int BaseGame::init() {
    cc::pipeline::GlobalDSManager::setDescriptorSetLayout();

    cc_load_all_plugins();

#if (CC_PLATFORM == CC_PLATFORM_ANDROID) && CC_SUPPORT_ADPF
    ADPFManager::getInstance().initialize();
#endif

#if CC_PLATFORM == CC_PLATFORM_WINDOWS || CC_PLATFORM == CC_PLATFORM_LINUX || CC_PLATFORM == CC_PLATFORM_QNX || CC_PLATFORM == CC_PLATFORM_MACOS
    // override default value
    //_windowInfo.x      = _windowInfo.x == -1 ? 0 : _windowInfo.x;
    //_windowInfo.y      = _windowInfo.y == -1 ? 0 : _windowInfo.y;
    #if CC_PLATFORM == CC_PLATFORM_WINDOWS
    // Windows sizes are physical pixels (see SDLHelper::init); scale the default as a DPI-unaware window would be.
    float dpi = 0;
    const float scale = SDL_GetDisplayDPI(0, nullptr, &dpi, nullptr) == 0 && dpi > 0 ? dpi / 96 : 1;
    #else
    const float scale = 1;
    #endif
    _windowInfo.width = _windowInfo.width == -1 ? static_cast<int>(std::lround(800 * scale)) : _windowInfo.width;
    _windowInfo.height = _windowInfo.height == -1 ? static_cast<int>(std::lround(600 * scale)) : _windowInfo.height;
    if (_windowInfo.flags == -1) {
        _windowInfo.flags = cc::ISystemWindow::CC_WINDOW_SHOWN |
                            cc::ISystemWindow::CC_WINDOW_INPUT_FOCUS;
        _windowInfo.flags |= cc::ISystemWindow::CC_WINDOW_RESIZABLE;
    }
    std::call_once(_windowCreateFlag, [&]() {
        ISystemWindowInfo info;
        info.title = _windowInfo.title;
        info.x = _windowInfo.x;
        info.y = _windowInfo.y;
        info.width = _windowInfo.width;
        info.height = _windowInfo.height;
        info.flags = _windowInfo.flags;

        ISystemWindowManager* windowMgr = CC_GET_PLATFORM_INTERFACE(ISystemWindowManager);
        windowMgr->createWindow(info);
    });

#endif

    if (_debuggerInfo.enabled) {
        setDebugIpAndPort(_debuggerInfo.address, _debuggerInfo.port, _debuggerInfo.pauseOnStart);
    }

    int ret = cc::CocosApplication::init();
    if (ret != 0) {
        return ret;
    }

    setXXTeaKey(_xxteaKey);
    runScript("jsb-adapter/web-adapter.js");
    runScript("main.js");
    return 0;
}
} // namespace cc
