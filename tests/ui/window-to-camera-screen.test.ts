import { UITransform } from '../../cocos/2d/framework/ui-transform';
import { Vec3 } from '../../cocos/core/math/vec3';
import { director } from '../../cocos/game/director';
import type { Camera } from '../../cocos/render-scene/scene/camera';
import { setCameraPresentedOnWindow } from '../../cocos/misc/camera-presentation';

test('a camera of a texture takes the window point only where its texture is presented over the window', () => {
    const main = director.root!.mainWindow!;
    jest.spyOn(main, 'width', 'get').mockReturnValue(1600);
    jest.spyOn(main, 'height', 'get').mockReturnValue(900);
    const point = new Vec3(main.width / 2, main.height / 4, 0);
    const out = new Vec3();

    // A camera of the window takes the point as it is.
    const onWindow = { window: main, width: main.width, height: main.height } as unknown as Camera;
    expect(UITransform.windowToCameraScreen(onWindow, point, out)).toBe(true);
    expect(out.equals(point)).toBe(true);

    // A camera of a texture has no place in the window.
    const onTexture = { window: { swapchain: null }, width: 800, height: 600 } as unknown as Camera;
    expect(UITransform.windowToCameraScreen(onTexture, point, out)).toBe(false);

    // Presented over the window, the point is as far into the texture as into the window.
    setCameraPresentedOnWindow(onTexture, true);
    expect(UITransform.windowToCameraScreen(onTexture, point, out)).toBe(true);
    expect(out.x).toBeCloseTo(400);
    expect(out.y).toBeCloseTo(150);

    setCameraPresentedOnWindow(onTexture, false);
    expect(UITransform.windowToCameraScreen(onTexture, point, out)).toBe(false);
});
