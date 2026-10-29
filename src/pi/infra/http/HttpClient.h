#pragma once

#include <string>
#include <vector>

#include <curl/curl.h>

namespace pi::infra {

// ---------------------------------------------------------------------------
// 技术设施: JSON POST (libcurl 同步封装).
// error 非空 = 传输层失败 (init 失败或 perform 失败), status 为 HTTP 状态码.
// ---------------------------------------------------------------------------
struct HttpResponse {
   long        status = 0;
   std::string body;
   std::string error;
};

inline auto httpWriteCb(char* p, size_t s, size_t n, void* ud) -> size_t {
   static_cast<std::string*>(ud)->append(p, s * n);
   return s * n;
}

inline auto httpPostJson(std::string const&              url,
                         std::string const&              body,
                         std::vector<std::string> const& headers,
                         long                            timeoutSec = 120)
   -> HttpResponse {
   HttpResponse r;

   CURL* curl = curl_easy_init();
   if (!curl) {
      r.error = "curl init failed";
      return r;
   }

   curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                    static_cast<long>(body.size()));
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &httpWriteCb);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);

   curl_slist* hlist = nullptr;
   for (auto const& h : headers) hlist = curl_slist_append(hlist, h.c_str());
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

   auto rc = curl_easy_perform(curl);
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
   curl_slist_free_all(hlist);
   curl_easy_cleanup(curl);

   if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
   return r;
}

} // namespace pi::infra
