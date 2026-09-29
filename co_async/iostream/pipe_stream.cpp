#include <co_async/awaiter/task.hpp>
#include <co_async/generic/condition_variable.hpp>
#include <co_async/generic/queue.hpp>
#include <co_async/iostream/pipe_stream.hpp>
#include <co_async/iostream/stream_base.hpp>
#include <co_async/platform/fs.hpp>

namespace co_async {

namespace {

struct PipeStreamBuffer {
    ConcurrentQueue<std::string> mChunks{64};
};

struct IPipeStream : Stream {
    Task<Expected<std::size_t>> raw_read(std::span<char> buffer) override {
        if (mPendingIndex == mPending.size()) {
#if CO_ASYNC_DEBUG
            auto e = co_await mPipe->mChunks.pop();
            if (e.has_error()) {
                std::cerr << "PipeStreamBuffer::pop(): " << e.error().message() << '\n';
                co_return CO_ASYNC_ERROR_FORWARD(e);
            }
            mPending = std::move(*e);
#else
            mPending = co_await co_await mPipe->mChunks.pop();
#endif
            mPendingIndex = 0;
        }
        auto n = std::min(buffer.size(), mPending.size() - mPendingIndex);
        std::memcpy(buffer.data(), mPending.data() + mPendingIndex, n);
        mPendingIndex += n;
        co_return n;
    }

    Task<> raw_close() override {
        mPending.clear();
        mPendingIndex = 0;
        mPipe.reset();
        co_return;
    }

    explicit IPipeStream(std::shared_ptr<PipeStreamBuffer> buffer)
        : mPipe(std::move(buffer)) {}

private:
    std::shared_ptr<PipeStreamBuffer> mPipe;
    std::string mPending;
    std::size_t mPendingIndex = 0;
};

struct OPipeStream : Stream {
    Task<Expected<std::size_t>>
    raw_write(std::span<char const> buffer) override {
        if (auto p = mPipe.lock()) [[likely]] {
            if (buffer.empty()) [[unlikely]] {
                co_return std::size_t(0);
            }
            co_await co_await p->mChunks.push(
                std::string(buffer.data(), buffer.size()));
            co_return buffer.size();
        } else {
            co_return std::errc::broken_pipe;
        }
    }

    Task<> raw_close() override {
        if (auto p = mPipe.lock()) {
            (void)co_await p->mChunks.push(std::string());
        }
        co_return;
    }

    ~OPipeStream() {
        if (auto p = mPipe.lock()) {
            p->mChunks.try_push(std::string());
        }
    }

    explicit OPipeStream(std::weak_ptr<PipeStreamBuffer> buffer)
        : mPipe(std::move(buffer)) {}

private:
    std::weak_ptr<PipeStreamBuffer> mPipe;
};

} // namespace

std::array<OwningStream, 2> pipe_stream() {
    auto pipePtr = std::make_shared<PipeStreamBuffer>();
    auto pipeWeakPtr = std::weak_ptr(pipePtr);
    return std::array{make_stream<IPipeStream>(std::move(pipePtr)),
                      make_stream<OPipeStream>(std::move(pipeWeakPtr))};
}

Task<Expected<>> pipe_forward(BorrowedStream &in, BorrowedStream &out) {
    while (true) {
        if (in.bufempty()) {
            if (!co_await in.fillbuf()) {
                break;
            }
        }
        auto n = co_await co_await out.write(in.peekbuf());
        if (n == 0) [[unlikely]] {
            co_return std::errc::broken_pipe;
        }
        in.seenbuf(n);
    }
    co_return {};
}

} // namespace co_async
