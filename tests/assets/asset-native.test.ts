import { readFileSync } from 'fs';
import { resolve } from 'path';
import { runInNewContext } from 'vm';
import { ModuleKind, ScriptTarget, transpileModule } from 'typescript';

test('native Asset exposes loader binary data through the public nativeAsset getter', () => {
    const module = { exports: {} as any };
    const compiled = transpileModule(readFileSync(resolve(__dirname, '../../cocos/asset/assets/asset.jsb.ts'), 'utf8'), {
        compilerOptions: { module: ModuleKind.CommonJS, target: ScriptTarget.ES2022 },
    });
    runInNewContext(compiled.outputText, {
        exports: module.exports,
        jsb: { Asset: class {} },
        require: (id: string) => ({
            '../../core': { cclegacy: {}, js: { createMap: () => Object.create(null) }, applyMixins() {} },
            '../../native-binding/decorators': { patch_cc_Asset() {} },
        }[id] ?? {}),
    });
    const asset = new module.exports.Asset();
    asset._ctor();
    expect(asset.nativeAsset).toBeNull();
    const bytes = new Uint8Array([0x50, 0x4b, 0x03, 0x04]).buffer;
    asset._nativeAsset = bytes;
    expect(asset.nativeAsset).toBe(bytes);
    asset._nativeAsset = null;
    expect(asset.nativeAsset).toBeNull();
});
