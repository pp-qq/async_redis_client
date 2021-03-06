
#pragma once

#include <pthread.h>

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <future>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>

#include <concurrent/mutex.h>
#include <exception/errno_exception.h>

#include <hiredis/async.h>
#include <hiredis/hiredis.h>
#include <uv.h>



struct RedisReplyDeleter {
    void operator()(redisReply *reply) noexcept {
        freeReplyObject(reply);
        return ;
    }
};

struct AsyncRedisClient {

    // 调用 Start() 之后, 这些值将只读.
    std::string host;
    in_port_t port = 6379;
    std::string passwd;

    size_t thread_num = 1;
    size_t conn_per_thread = 3;

public:
    using req_callback_t = std::function<void(redisReply *reply)/* noexcept */>;
    using redisReply_unique_ptr_t = std::unique_ptr<redisReply, RedisReplyDeleter>;

public:
    ~AsyncRedisClient() noexcept;

    /**
     * 启动 AsyncRedisClient. 在此之后可以通过 AsyncRedisClient::Execute() 来执行请求.
     *
     * Start() 不是线程安全的(因为认为 Start() 相当于初始化函数, 没必要线程安全).
     *
     * Start() 只应该调用一次, 多次调用行为未定义.
     */
    void Start();

    /* 只有这里的方法才是线程安全的.
     * 意味着可以在不同的线程同时调用 `Stop()`, 或者 `Execute()`. 但是不能在一个线程中调用 `Stop()`, 另外一个线程
     * 调用 `~AsyncRedisClient()`.
     */
public:
    /**
     * 停止 AsyncRedisClient.
     * 此后当前 client 不再接受新的请求. 正在处理的请求回调会继续执行, 尚未处理的请求回调会接受 nullptr reply.
     *
     * Stop() 之后的 AsyncRedisClient 恢复到初始状态, 此时可以修改参数再一次 Start().
     */
    void Stop() {
        DoStopOrJoin(ClientStatus::kStop);
        return ;
    }

    /**
     * 停止 AsyncRedisClient.
     * 此后当前 client 不再接受新的请求. 正在处理的请求回调会继续执行, 尚未处理的请求回调仍会正常执行.
     *
     * Join() 之后的 AsyncRedisClient 恢复到初始状态, 此时可以修改参数再一次 Start().
     */
    void Join() {
        DoStopOrJoin(ClientStatus::kJoin);
        return ;
    }

    /**
     * 执行一个 redis 请求.
     *
     * 若该函数抛出异常, 则表明 request 不会被当前 Client 执行. 否则
     *
     * 若 request 成功处理, callback(reply) 中的 reply 指向着响应, reply 指向着的响应会在 callback() 返回之后
     * 释放. 若 request 未被成功处理, 执行 callback(nullptr).
     *
     * callback() MUST noexcept, 若 callback() 抛出了异常, 则会直接 std::terminate().
     *
     * TODO(ppqq): 增加 host, port 参数, 表明在指定的 redis 实例上执行请求.
     * TODO(ppqq): 增加超时参数. 当超时时, 以 nullptr reply 调用回调. 倒是可以通过 future.wait() 来实现超时.
     */
    void Execute(const std::vector<std::string> &cmd, const req_callback_t &cb) {
        std::unique_ptr<RedisRequest> req(new RedisRequest(cmd, cb));
        Execute(req);
        return ;
    }

    void Execute(const std::vector<std::string> &cmd, req_callback_t &&cb) {
        std::unique_ptr<RedisRequest> req(new RedisRequest(cmd, std::move(cb)));
        Execute(req);
        return ;
    }

    void Execute(std::vector<std::string> &&cmd, const req_callback_t &cb) {
        std::unique_ptr<RedisRequest> req(new RedisRequest(std::move(cmd), cb));
        Execute(req);
        return ;
    }

    void Execute(std::vector<std::string> &&cmd, req_callback_t &&cb) {
        std::unique_ptr<RedisRequest> req(new RedisRequest(std::move(cmd), std::move(cb)));
        Execute(req);
        return ;
    }

    void Execute(const std::shared_ptr<std::vector<std::string>> &request,
                 const std::shared_ptr<req_callback_t> &callback) {
        Execute(*request, *callback);
        return ;
    }

    std::future<redisReply_unique_ptr_t> Execute(const std::shared_ptr<std::vector<std::string>> &request) {
        return Execute(*request);
    }

    std::future<redisReply_unique_ptr_t> Execute(const std::vector<std::string> &cmd);
    std::future<redisReply_unique_ptr_t> Execute(std::vector<std::string> &&cmd);


/* 本来这些都是 private 就行了.
 *
 * 但是我想重载个 operator<<(ostream &out, ClientStatus); 本来是把这个重载当作是 static member, 然后编译报错.
 * 貌似只能作为 non-member, 这样子的话, ClientStatus 也就必须得是 public 了.
 */
public:
    using status_t = unsigned int;

    enum class ClientStatus : status_t {
        kInitial = 0,
        kStarted,
        kStop,
        kJoin
    };

    enum class WorkThreadStatus : status_t {
        kUnknown = 0,
        kExiting,
        kRunning
    };

    struct RedisRequest {
        std::vector<std::string> cmd;
        req_callback_t callback;

    public:
        RedisRequest() noexcept = default;

        RedisRequest(const std::vector<std::string> &cmd_arg, const req_callback_t &callback_arg):
            cmd(cmd_arg),
            callback(callback_arg) {
        }

        RedisRequest(const std::vector<std::string> &cmd_arg, req_callback_t &&callback_arg):
            cmd(cmd_arg),
            callback(std::move(callback_arg)) {
        }

        RedisRequest(std::vector<std::string> &&cmd_arg, const req_callback_t &callback_arg):
            cmd(std::move(cmd_arg)),
            callback(callback_arg) {
        }

        RedisRequest(std::vector<std::string> &&cmd_arg, req_callback_t &&callback_arg):
            cmd(std::move(cmd_arg)),
            callback(std::move(callback_arg)) {
        }

        RedisRequest(const RedisRequest &) = default;
        RedisRequest(RedisRequest &&other):
            cmd(std::move(other.cmd)),
            callback(std::move(other.callback)) {
        }

        RedisRequest& operator=(const RedisRequest &) = default;
        RedisRequest& operator=(RedisRequest &&other) {
            cmd = std::move(other.cmd);
            callback = std::move(other.callback);
            return *this;
        }

        void Fail() noexcept {
            if (callback) {
                callback(nullptr);
            }
            return ;
        }

        void Success(redisReply *reply) noexcept {
            if (callback) {
                callback(reply);
            }
            return ;
        }
    };

    struct WorkThread {
        bool started = false;
        std::thread thread;

        // NOTE: 总是先 lock vec_mux 再 lock handle_mux.
        std::mutex vec_mux;
        /* request_vec 的内存是由 work thread 来分配.
         *
         * 对于其他线程而言, 其检测到若 request_vec 为 nullptr, 则表明对应的 work thread 不再工作, 此时不能往
         * request_vec 中加入请求. 反之, 则表明 work thread 正常工作, 此时可以压入元素.
         */
        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> request_vec;

        std::shared_mutex handle_mux;
        /* 不变量 3: 若 async_handle != nullptr, 则表明 async_handle 指向着的 uv_async_t 已经被初始化, 此时
         * 对其调用 uv_async_send() 不会触发 SIGSEGV.
         *
         * 其实这里可以使用读写锁, 因为 uv_async_send() 是线程安全的, 但是 uv_close(), uv_async_init() 这些
         * 并不是. 也即在执行 uv_async_send() 之前加读锁, 其他操作加写锁.
         */
        uv_async_t *async_handle = nullptr;


    public:
        void AsyncSend() noexcept {
            handle_mux.lock_shared();
            if (async_handle) {
                uv_async_send(async_handle); // 当 send() 失败了怎么办???
            }
            handle_mux.unlock_shared();
            return ;
        }

        /*
         * 将 req 表示的请求追加到当前 work thread 中.
         *
         * 若抛出异常, 则表明追加失败, 此时 req 引用的对象没有任何变化. 若未抛出异常, 则根据 req
         * 是否为空来判断请求是否成功追加, 即当为空时, 表明请求成功追加到当前 work thread 中.
         */
        void AddRequest(std::unique_ptr<RedisRequest> &req);
    };

private:
    std::atomic<ClientStatus> status_{ClientStatus::kInitial}; // lock-free
    std::atomic_uint seq_num{0};
    std::unique_ptr<std::vector<WorkThread>> work_threads_;

private:
    /* 若成功, 则 req 指向的内存由 AsyncRedisClient 来管理. 若失败, 则抛出异常, 并且 req 保持不变.
     */
    void Execute(std::unique_ptr<RedisRequest> &req);

private:
    ClientStatus GetStatus() noexcept {
        return status_.load(std::memory_order_relaxed);
    }

    void SetStatus(ClientStatus status) noexcept {
        status_.store(status, std::memory_order_relaxed);
        return ;
    }

    void JoinAllThread() noexcept {
        for (WorkThread &work_thread : *work_threads_) {
            if (!work_thread.started)
                continue ;

            work_thread.thread.join(); // join() 理论上不会抛出异常的.
        }
    }

    void DoStopOrJoin(ClientStatus op);
private:
    static void WorkThreadMain(AsyncRedisClient *client, size_t idx, std::promise<void> *p) noexcept;

    static void OnAsyncHandle(uv_async_t* handle) noexcept;
    static void OnRedisReply(redisAsyncContext *c, void *reply, void *privdata) noexcept;
};

inline std::ostream& operator<<(std::ostream &out, AsyncRedisClient::ClientStatus status) {
    out << static_cast<AsyncRedisClient::status_t>(status);
    return out;
}

