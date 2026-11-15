/*
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 https://www.cocos.com/

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
*/

import { Vec3, cclegacy } from '../core';
import type { Camera } from '../render-scene/scene/camera';
import type { Root } from '../root';

// Kept apart from the camera component and the UI, which both use it: neither may load the other.

/** Cameras that take their system window's pointer though they render to a texture. */
const presentedCameras = new WeakSet<Camera>();

/**
 * @en Lets a camera that renders to a texture take the pointer of its system window, as a camera rendering to the
 * window does: for a texture that is presented stretched over the whole window. See `Camera.presentedOnWindow`.
 * @zh 让渲染到贴图的相机像渲染到窗口的相机一样接收系统窗口的指针：用于贴图被拉伸铺满整个窗口呈现的情况。
 * 参见 `Camera.presentedOnWindow`。
 * @engineInternal
 */
export function setCameraPresentedOnWindow (camera: Camera, presented: boolean): void {
    if (presented) presentedCameras.add(camera);
    else presentedCameras.delete(camera);
}

/**
 * @en Converts a point of the system window into the screen space of a camera, which the camera's `screenToWorld`
 * takes. See `UITransform.windowToCameraScreen`.
 * @zh 把系统窗口中的点转换到相机自己的屏幕空间。参见 `UITransform.windowToCameraScreen`。
 * @engineInternal
 */
export function windowToCameraScreen (camera: Camera, windowPos: Readonly<Vec3>, out: Vec3): boolean {
    const window = camera.window;
    if (!window || window.swapchain) {
        Vec3.copy(out, windowPos);
        return true;
    }
    const main = (cclegacy.director?.root as Root | undefined)?.mainWindow;
    if (!main || !presentedCameras.has(camera)) { return false; }
    // The window's size is pre-rotated; a camera's is not.
    const rotated = !!main.swapchain && main.swapchain.surfaceTransform % 2 === 1;
    const width = rotated ? main.height : main.width;
    const height = rotated ? main.width : main.height;
    Vec3.set(out, windowPos.x * camera.width / width, windowPos.y * camera.height / height, windowPos.z);
    return true;
}
