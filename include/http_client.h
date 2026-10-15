#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "http.h"
#include "tcp.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <map>
#include <string>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <memory>
#include <deque>
#include <cstring>

#define EC801E_HTTP_EVENT_HEADERS_RECEIVED (1 << 0)
#define EC801E_HTTP_EVENT_BODY_RECEIVED (1 << 1)
#define EC801E_HTTP_EVENT_ERROR (1 << 2)
#define EC801E_HTTP_EVENT_COMPLETE (1 << 3)

class NetworkInterface;

class HttpClient : public Http {
public:
    HttpClient(NetworkInterface* network, int connect_id = 0);
    ~HttpClient();

    void SetTimeout(int timeout_ms) override;
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override;
    NetworkResult<> Open(const std::string& method, const std::string& url) override;
    void Close() override;
    NetworkResult<int> Read(char* buffer, size_t buffer_size) override;
    NetworkResult<int> Write(const char* buffer, size_t buffer_size) override;
    
    // Keep-Alive support
    void SetKeepAlive(bool enable) override;
    bool IsConnectionReusable(const std::string& host, int port) const;

    NetworkResult<int> GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override;
    std::string ReadAll() override;

private:
    // Queued slice of response body with its own read offset.
    struct DataChunk {
        std::string data;
        size_t offset = 0;
        
        DataChunk(std::string&& d) : data(std::move(d)), offset(0) {}
        DataChunk(const std::string& d) : data(d), offset(0) {}
        
        size_t available() const {
            return data.size() - offset;
        }
        
        size_t read(char* buffer, size_t size) {
            size_t bytes_to_read = std::min(size, available());
            if (bytes_to_read > 0) {
                memcpy(buffer, data.data() + offset, bytes_to_read);
                offset += bytes_to_read;
            }
            return bytes_to_read;
        }
        
        bool empty() const {
            return offset >= data.size();
        }
    };

    // Header maps are keyed by the lowercase name; the original spelling is
    // kept for writing request headers.
    struct HeaderEntry {
        std::string original_key;
        std::string value;
        
        HeaderEntry() = default;
        HeaderEntry(const std::string& key, const std::string& val) 
            : original_key(key), value(val) {}
    };

    NetworkInterface* network_;
    int connect_id_;
    std::unique_ptr<Tcp> tcp_;
    EventGroupHandle_t event_group_handle_;
    // Guards parser state. Lock order: mutex_ before read_mutex_.
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    
    // Guards body_chunks_; cv_ and write_cv_ wait on it.
    std::mutex read_mutex_;
    std::deque<DataChunk> body_chunks_;
    std::condition_variable write_cv_;
    const size_t MAX_BODY_CHUNKS_SIZE = 8192;
    
    int status_code_ = -1;
    int timeout_ms_ = 30000;
    std::string rx_buffer_;
    std::map<std::string, HeaderEntry> headers_;
    std::string url_;
    std::string method_;
    std::string protocol_;
    std::string host_;
    std::string path_;
    int port_ = 80;
    // Endpoint of the open transport; host_/port_ already describe the next request.
    std::string connected_protocol_;
    std::string connected_host_;
    int connected_port_ = 0;
    std::optional<std::string> content_ = std::nullopt;
    std::map<std::string, HeaderEntry> response_headers_;
    
    size_t content_length_ = 0;
    size_t total_body_received_ = 0;
    // Shared between the receive task and readers.
    std::atomic<bool> eof_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> headers_received_{false};
    std::atomic<bool> connection_error_{false};
    std::atomic<bool> server_keep_alive_{false};
    // Reason reported once connection_error_ is set; written before the flag.
    NetworkError response_error_{};
    bool request_chunked_ = false;
    bool response_chunked_ = false;
    bool keep_alive_ = false;
    
    enum class ParseState {
        STATUS_LINE,
        HEADERS,
        BODY,
        CHUNK_SIZE,
        CHUNK_DATA,
        CHUNK_TRAILER,
        COMPLETE
    };
    ParseState parse_state_ = ParseState::STATUS_LINE;
    size_t chunk_size_ = 0;
    size_t chunk_received_ = 0;

    bool ParseUrl(const std::string& url);
    std::string BuildHttpRequest();
    void OnTcpData(const std::string& data);
    void OnTcpDisconnected();
    void ProcessReceivedData();
    bool TakeLine(std::string* line);
    bool ParseStatusLine(const std::string& line);
    bool ParseHeaderLine(const std::string& line);
    bool FinishHeaders();
    void ParseRegularBody();
    bool ParseChunkSize(const std::string& chunk_size_line, size_t* chunk_size);
    void CompleteResponse();
    void SetError();
    void WakeReaders();
    
    void AddBodyData(const std::string& data);
    void AddBodyData(std::string&& data);
    
    bool IsDataComplete() const;
    
    // Clears per-response state while keeping the connection for reuse.
    void ResetRequestState();
};

#endif
