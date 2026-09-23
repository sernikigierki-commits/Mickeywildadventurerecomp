#include "mickey_http.h"
#if defined(__ANDROID__)
#include "psx_sdl.h"
#include <jni.h>

MickeyHttpResponse mickey_http_request(const char* url, const char* post,
                                       const char* content_type) {
    MickeyHttpResponse response;
    if (!url || !url[0]) return response;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return response;
    jclass activity_class = env->GetObjectClass(activity);
    jmethodID method = env->GetStaticMethodID(activity_class, "httpRequest",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Lcom/kacper/mickeywildadventure/MickeyActivity$HttpResult;");
    if (method) {
        jstring jurl = env->NewStringUTF(url);
        jstring jpost = post ? env->NewStringUTF(post) : nullptr;
        jstring jtype = content_type ? env->NewStringUTF(content_type) : nullptr;
        jobject result = env->CallStaticObjectMethod(activity_class, method,
                                                       jurl, jpost, jtype);
        if (!env->ExceptionCheck() && result) {
            jclass result_class = env->GetObjectClass(result);
            jfieldID status = env->GetFieldID(result_class, "status", "I");
            jfieldID body = env->GetFieldID(result_class, "body", "[B");
            if (status && body) {
                response.status = env->GetIntField(result, status);
                auto bytes = static_cast<jbyteArray>(env->GetObjectField(result, body));
                if (bytes) {
                    jsize size = env->GetArrayLength(bytes);
                    if (size >= 0 && size <= 16 * 1024 * 1024) {
                        response.body.resize(static_cast<size_t>(size));
                        env->GetByteArrayRegion(bytes, 0, size,
                            reinterpret_cast<jbyte*>(response.body.data()));
                    }
                    env->DeleteLocalRef(bytes);
                }
            }
            env->DeleteLocalRef(result_class);
            env->DeleteLocalRef(result);
        }
        if (jurl) env->DeleteLocalRef(jurl);
        if (jpost) env->DeleteLocalRef(jpost);
        if (jtype) env->DeleteLocalRef(jtype);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        response = {};
    }
    if (activity_class) env->DeleteLocalRef(activity_class);
    env->DeleteLocalRef(activity);
    return response;
}
#else
#include <curl/curl.h>

namespace {
size_t receive_body(char* data, size_t size, size_t count, void* context) {
    constexpr size_t limit = 16 * 1024 * 1024;
    auto& body = *static_cast<std::string*>(context);
    if (size != 0 && count > (limit - body.size()) / size)
        return 0;
    const size_t bytes = size * count;
    try {
        body.append(data, bytes);
    } catch (...) {
        return 0;
    }
    return bytes;
}
}

MickeyHttpResponse mickey_http_request(const char* url, const char* post,
                                      const char* content_type) {
    MickeyHttpResponse response;
    static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (!url || !*url || initialized != CURLE_OK)
        return response;
    CURL* curl = curl_easy_init();
    if (!curl)
        return response;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    // These options are available in Bullseye's libcurl 7.74.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, long(CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, long(CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MickeyWildAdventureRecomp/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_slist* headers = nullptr;
    if (post && *post) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
        if (content_type && *content_type) {
            const std::string header = std::string("Content-Type: ") + content_type;
            headers = curl_slist_append(nullptr, header.c_str());
            if (!headers) {
                curl_easy_cleanup(curl);
                return response;
            }
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
    }
    if (curl_easy_perform(curl) == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);
    } else {
        response.body.clear();
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}
#endif
