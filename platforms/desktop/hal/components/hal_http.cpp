/*
 * Desktop HTTP implementation using libcurl.
 * Each call creates/destroys its own CURL handle → thread-safe.
 */
#include "../hal_desktop.h"
#include <curl/curl.h>
#include <mooncake_log.h>
#include <cstdio>

static const std::string _tag = "hal-http";

static size_t _write_file_cb(char* ptr, size_t size, size_t nmemb, FILE* fp)
{
    return fwrite(ptr, size, nmemb, fp) * size;
}

static size_t _write_cb(char* ptr, size_t size, size_t nmemb, std::string* data)
{
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

static struct curl_slist* _build_headers(const std::vector<std::pair<std::string, std::string>>& headers)
{
    struct curl_slist* list = nullptr;
    for (auto& [k, v] : headers) {
        list = curl_slist_append(list, (k + ": " + v).c_str());
    }
    return list;
}

hal::HalBase::HttpResponse_t HalDesktop::httpGet(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResponse_t resp;
    CURL* curl = curl_easy_init();
    if (!curl) return resp;

    struct curl_slist* hlist = _build_headers(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status = (int)http_code;
        resp.ok     = (http_code >= 200 && http_code < 300);
    } else {
        mclog::tagWarn(_tag, "GET {} failed: {}", url, curl_easy_strerror(res));
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return resp;
}

hal::HalBase::HttpResponse_t HalDesktop::httpPost(
    const std::string& url,
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResponse_t resp;
    CURL* curl = curl_easy_init();
    if (!curl) return resp;

    struct curl_slist* hlist = _build_headers(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status = (int)http_code;
        resp.ok     = (http_code >= 200 && http_code < 300);
    } else {
        mclog::tagWarn(_tag, "POST {} failed: {}", url, curl_easy_strerror(res));
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return resp;
}

bool HalDesktop::httpGetToFile(
    const std::string& url,
    const std::string& filePath,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    FILE* fp = fopen(filePath.c_str(), "wb");
    if (!fp) {
        mclog::tagWarn(_tag, "open {} failed", filePath);
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        return false;
    }

    struct curl_slist* hlist = _build_headers(headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    bool ok = false;
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        ok = (http_code >= 200 && http_code < 300);
    } else {
        mclog::tagWarn(_tag, "GET->file {} failed: {}", url, curl_easy_strerror(res));
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    fclose(fp);
    return ok;
}
