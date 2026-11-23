#include "vrp/worker/transfer.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"

namespace vrp::worker {
namespace {

using namespace std::chrono_literals;

constexpr auto kFirstRetry = 1s;
constexpr auto kMaxRetry = 15s;
constexpr auto kMaxOutage = 10min;  // so that a job cannot wait forever
constexpr auto kRetryPoll = 100ms;
constexpr int kMaxUrlRefreshes = 3;
constexpr auto kConnectSeconds = 10L;
constexpr auto kStalledSeconds = 30L;

struct CurlDeleter {
  void operator()(CURL* curl) const { curl_easy_cleanup(curl); }
};
struct HeadersDeleter {
  void operator()(curl_slist* headers) const { curl_slist_free_all(headers); }
};

// One request's outcome; `stopped` wins over `outcome`.
struct Outcome {
  HttpOutcome outcome = HttpOutcome::kOk;
  bool stopped = false;
  std::string message;
};

struct Download {
  std::string* body;
  std::uint64_t max_bytes;
};

struct Upload {
  std::span<const std::byte> body;
  std::size_t sent = 0;
};

// curl calls these through `char*`, so the signatures must match exactly.
// NOLINTNEXTLINE(readability-non-const-parameter)
std::size_t Receive(char* data, std::size_t size, std::size_t count,
                    void* user) {
  auto& download = *static_cast<Download*>(user);
  const std::size_t bytes = size * count;
  if (download.body->size() + bytes > download.max_bytes) {
    return 0;  // curl fails the transfer with CURLE_WRITE_ERROR
  }
  download.body->append(data, bytes);
  return bytes;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
std::size_t Discard(char* /*data*/, std::size_t size, std::size_t count,
                    void* /*user*/) {
  return size * count;
}

std::size_t Send(char* buffer, std::size_t size, std::size_t count,
                 void* user) {
  auto& upload = *static_cast<Upload*>(user);
  const std::size_t bytes =
      std::min(size * count, upload.body.size() - upload.sent);
  if (bytes > 0) {
    std::memcpy(buffer, upload.body.subspan(upload.sent).data(), bytes);
  }
  upload.sent += bytes;
  return bytes;
}

int OnProgress(void* user, curl_off_t /*download_total*/,
               curl_off_t /*downloaded*/, curl_off_t /*upload_total*/,
               curl_off_t /*uploaded*/) {
  return (*static_cast<const std::function<bool()>*>(user))() ? 1 : 0;
}

// A bad certificate will not fix itself, so it is not retried.
bool IsPermanent(CURLcode code) {
  return code == CURLE_PEER_FAILED_VERIFICATION ||
         code == CURLE_SSL_CACERT_BADFILE || code == CURLE_URL_MALFORMAT ||
         code == CURLE_UNSUPPORTED_PROTOCOL || code == CURLE_FILESIZE_EXCEEDED;
}

Outcome Finish(CURL* curl, CURLcode code) {
  if (code == CURLE_ABORTED_BY_CALLBACK) {
    return {.outcome = HttpOutcome::kOk, .stopped = true, .message = ""};
  }
  if (code == CURLE_WRITE_ERROR) {  // only `Receive` refuses data
    return {.outcome = HttpOutcome::kTooLarge,
            .stopped = false,
            .message = "the object is larger than expected"};
  }
  long status = 0;  // NOLINT(google-runtime-int): curl's type
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (code == CURLE_OK || code == CURLE_HTTP_RETURNED_ERROR) {
    const HttpOutcome outcome = ClassifyHttpStatus(static_cast<int>(status));
    return {.outcome = outcome,
            .stopped = false,
            .message = outcome == HttpOutcome::kOk
                           ? ""
                           : "the object store answered HTTP " +
                                 std::to_string(status)};
  }
  return {
      .outcome = IsPermanent(code) ? HttpOutcome::kFailed : HttpOutcome::kRetry,
      .stopped = false,
      .message =
          std::string("the transfer failed: ") + curl_easy_strerror(code)};
}

// Options common to GET and PUT.
void SetCommon(CURL* curl, const std::string& url,
               const std::filesystem::path& ca_certificate,
               std::function<bool()>* stop_requested) {
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectSeconds);
  // A transfer that moves nothing for this long has stalled.
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kStalledSeconds);
  if (!ca_certificate.empty()) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_certificate.string().c_str());
  }
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, stop_requested);
}

// Runs `request` until it succeeds, fails for good, or is stopped.
template <typename Request>
core::Status WithRetries(const ObjectAccess& access, const Request& request) {
  auto delay =
      std::chrono::duration_cast<std::chrono::milliseconds>(kFirstRetry);
  int refreshes = 0;
  std::optional<std::chrono::steady_clock::time_point> give_up_at;
  std::string last_reason;
  while (true) {
    if (access.stop_requested()) {
      return core::CancelledError("stopped during a transfer");
    }
    const Outcome outcome = request(access.url());
    if (outcome.stopped) {
      return core::CancelledError("stopped during a transfer");
    }
    switch (outcome.outcome) {
      case HttpOutcome::kOk:
        return {};
      case HttpOutcome::kFailed:
        return core::InternalError(outcome.message);
      case HttpOutcome::kTooLarge:
        return core::DataLossError(outcome.message);
      case HttpOutcome::kExpired:
        if (++refreshes > kMaxUrlRefreshes) {
          return core::InternalError(outcome.message);
        }
        VRP_RETURN_IF_ERROR(access.refresh_url());
        break;
      case HttpOutcome::kRetry: {
        const auto now = std::chrono::steady_clock::now();
        give_up_at = give_up_at.value_or(now + kMaxOutage);
        if (now >= *give_up_at) {
          return core::InternalError(
              "the object store stayed unavailable for 10 minutes: " +
              outcome.message);
        }
        if (outcome.message != last_reason) {
          last_reason = outcome.message;
          access.warn("the object store is unavailable, retrying: " +
                      outcome.message);
        }
        const auto retry_at = now + delay;
        while (std::chrono::steady_clock::now() < retry_at) {
          if (access.stop_requested()) {
            return core::CancelledError("stopped during a transfer");
          }
          std::this_thread::sleep_for(kRetryPoll);
        }
        delay = std::min<std::chrono::milliseconds>(delay * 2, kMaxRetry);
        break;
      }
    }
  }
}

}  // namespace

HttpOutcome ClassifyHttpStatus(int status) {
  if (status >= 200 && status < 300) {
    return HttpOutcome::kOk;
  }
  if (status == 403) {
    return HttpOutcome::kExpired;
  }
  if (status == 408 || status == 429 || status >= 500) {
    return HttpOutcome::kRetry;
  }
  return HttpOutcome::kFailed;
}

core::StatusOr<std::string> ObjectTransfer::Get(const ObjectAccess& access,
                                                std::uint64_t max_bytes) const {
  const std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
  if (curl == nullptr) {
    return core::InternalError("cannot start a transfer");
  }
  std::string body;
  VRP_RETURN_IF_ERROR(WithRetries(access, [&](const std::string& url) {
    body.clear();
    Download download{.body = &body, .max_bytes = max_bytes};
    curl_easy_reset(curl.get());
    std::function<bool()> stop_requested = access.stop_requested;
    SetCommon(curl.get(), url, ca_certificate_, &stop_requested);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &download);
    return Finish(curl.get(), curl_easy_perform(curl.get()));
  }));
  metrics_->bytes_downloaded.Increment(static_cast<double>(body.size()));
  return body;
}

core::Status ObjectTransfer::Put(const ObjectAccess& access,
                                 std::span<const std::byte> body) const {
  const std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
  // Without this, curl waits for a `100 Continue` on larger bodies.
  const std::unique_ptr<curl_slist, HeadersDeleter> headers(
      curl_slist_append(nullptr, "Expect:"));
  if (curl == nullptr || headers == nullptr) {
    return core::InternalError("cannot start a transfer");
  }
  VRP_RETURN_IF_ERROR(WithRetries(access, [&](const std::string& url) {
    Upload upload{.body = body};
    curl_easy_reset(curl.get());
    std::function<bool()> stop_requested = access.stop_requested;
    SetCommon(curl.get(), url, ca_certificate_, &stop_requested);
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, Send);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl.get(), CURLOPT_INFILESIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Discard);
    return Finish(curl.get(), curl_easy_perform(curl.get()));
  }));
  metrics_->bytes_uploaded.Increment(static_cast<double>(body.size()));
  return {};
}

}  // namespace vrp::worker
