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
#include "cocos/bindings/manual/jsb_global.h"
#include "cocos/platform/FileUtils.h"
#include "cocos/platform/java/jni/JniHelper.h"

#include <regex>

#ifndef JCLS_CANVASIMPL
    #define JCLS_CANVASIMPL "com/cocos/lib/CanvasRenderingContext2DImpl"
#endif

static ccstd::unordered_map<ccstd::string, ccstd::string> gFontFamilyNameMap;

const ccstd::unordered_map<ccstd::string, ccstd::string> &getFontFamilyNameMap() {
    return gFontFamilyNameMap;
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

        fontFilePath = cc::FileUtils::getInstance()->fullPathForFilename(fontFilePath);
        if (fontFilePath.empty()) {
            SE_LOGE("Font (%s) doesn't exist!", fontFilePath.c_str());
            return true;
        }

        cc::JniHelper::callStaticVoidMethod(JCLS_CANVASIMPL, "loadTypeface", originalFamilyName, fontFilePath);

        s.rval().setString(originalFamilyName);

        return true;
    }

    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(jsbLoadFont)

#ifndef JCLS_FILEPICKER
    #define JCLS_FILEPICKER "com/cocos/lib/CocosFilePicker"
#endif

static se::Value gFilePickerCallback;

static void completeFileSelection(const se::Value &callback, const ccstd::vector<ccstd::string> &paths, const char *error) {
    se::AutoHandleScope scope;
    se::HandleObject array(se::Object::createArrayObject(paths.size()));
    for (uint32_t i = 0; i < paths.size(); ++i) array->setArrayElement(i, se::Value(paths[i]));
    callback.toObject()->call({se::Value(array), error ? se::Value(error) : se::Value::Null}, nullptr);
}

static bool jsbSelectFiles(se::State &s) {
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() == 2 && args[0].isString() && args[1].isObject() && args[1].toObject()->isFunction(), false, "Expected a file extension and callback");
    if (!gFilePickerCallback.isUndefined()) {
        completeFileSelection(args[1], {}, "A file selection is already open");
        return true;
    }
    gFilePickerCallback.setObject(args[1].toObject(), true);
    cc::JniHelper::callStaticVoidMethod(JCLS_FILEPICKER, "select", args[0].toString());
    return true;
}
SE_BIND_FUNC(jsbSelectFiles)

// Called by CocosFilePicker on the game thread.
extern "C" JNIEXPORT void JNICALL Java_com_cocos_lib_CocosFilePicker_onFilesSelectedNative(JNIEnv *env, jclass /*unused*/, jobjectArray paths, jstring error) { // NOLINT
    if (gFilePickerCallback.isUndefined()) return;
    se::Value callback = std::move(gFilePickerCallback);
    gFilePickerCallback.setUndefined();
    ccstd::vector<ccstd::string> result;
    const jsize count = env->GetArrayLength(paths);
    for (jsize i = 0; i < count; ++i) {
        auto *path = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        result.push_back(cc::JniHelper::jstring2string(path));
        env->DeleteLocalRef(path);
    }
    const ccstd::string message = error ? cc::JniHelper::jstring2string(error) : "";
    completeFileSelection(callback, result, error ? message.c_str() : nullptr);
}

bool register_platform_bindings(se::Object * /*obj*/) { // NOLINT(readability-identifier-naming)
    __jsbObj->defineFunction("loadFont", _SE(jsbLoadFont));
    __jsbObj->defineFunction("selectFiles", _SE(jsbSelectFiles));
    se::ScriptEngine::getInstance()->addBeforeCleanupHook([]() { gFilePickerCallback.setUndefined(); });
    return true;
}