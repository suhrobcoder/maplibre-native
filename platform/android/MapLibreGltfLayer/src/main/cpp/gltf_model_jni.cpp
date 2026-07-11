// JNI bridge for org.maplibre.gltf.GltfModel. Loading is pure CPU and safe to
// call from any thread (callers should avoid the main thread for large files).

#include "gltf_model.hpp"

#include <jni.h>

namespace {

void throwIoException(JNIEnv* env, const std::string& message) {
    jclass cls = env->FindClass("java/io/IOException");
    if (cls) env->ThrowNew(cls, message.c_str());
}

maplibre_gltf::Model* asModel(jlong handle) {
    return reinterpret_cast<maplibre_gltf::Model*>(handle);
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_maplibre_gltf_GltfModel_nativeLoadFromFile(JNIEnv* env, jclass, jstring jpath) {
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    std::string error;
    auto model = maplibre_gltf::loadModelFromFile(path, error);
    env->ReleaseStringUTFChars(jpath, path);
    if (!model) {
        throwIoException(env, error);
        return 0;
    }
    return reinterpret_cast<jlong>(model.release());
}

JNIEXPORT jlong JNICALL Java_org_maplibre_gltf_GltfModel_nativeLoadFromBytes(JNIEnv* env, jclass, jbyteArray jbytes) {
    jsize size = env->GetArrayLength(jbytes);
    jbyte* bytes = env->GetByteArrayElements(jbytes, nullptr);
    std::string error;
    auto model = maplibre_gltf::loadModelFromMemory(
        reinterpret_cast<const uint8_t*>(bytes), static_cast<size_t>(size), "", error);
    env->ReleaseByteArrayElements(jbytes, bytes, JNI_ABORT);
    if (!model) {
        throwIoException(env, error);
        return 0;
    }
    return reinterpret_cast<jlong>(model.release());
}

JNIEXPORT void JNICALL Java_org_maplibre_gltf_GltfModel_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete asModel(handle);
}

JNIEXPORT jint JNICALL Java_org_maplibre_gltf_GltfModel_nativeDrawableCount(JNIEnv*, jclass, jlong handle) {
    return static_cast<jint>(asModel(handle)->drawables.size());
}

JNIEXPORT jint JNICALL Java_org_maplibre_gltf_GltfModel_nativeMaterialCount(JNIEnv*, jclass, jlong handle) {
    return static_cast<jint>(asModel(handle)->materials.size());
}

JNIEXPORT jint JNICALL Java_org_maplibre_gltf_GltfModel_nativeTextureCount(JNIEnv*, jclass, jlong handle) {
    return static_cast<jint>(asModel(handle)->textures.size());
}

JNIEXPORT jlong JNICALL Java_org_maplibre_gltf_GltfModel_nativeVertexCount(JNIEnv*, jclass, jlong handle) {
    return static_cast<jlong>(asModel(handle)->totalVertices());
}

JNIEXPORT jlong JNICALL Java_org_maplibre_gltf_GltfModel_nativeIndexCount(JNIEnv*, jclass, jlong handle) {
    return static_cast<jlong>(asModel(handle)->totalIndices());
}

} // extern "C"
