#include "log_worker.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>

namespace nte::damage_log {
namespace {

constexpr std::size_t kMaximumQueuedEvents = 8192;

struct DeleteCommand { std::string report_id; };
using Command = std::variant<ReportOpen, EncounterOpen, LoggedDamage,
    EncounterClose, ReportClose, DeleteCommand>;

}  // namespace

struct DamageLogWorker::Impl {
    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    std::filesystem::path root;
    mutable std::mutex mutex;
    std::condition_variable available;
    std::deque<Command> commands;
    std::thread thread;
    LogWorkerStatus status;
    bool stopping{};
    std::size_t queued_events{};

    bool SubmitControl(Command command) {
        std::scoped_lock lock(mutex);
        if (!status.running || stopping) return false;
        commands.push_back(std::move(command));
        status.queue_depth = commands.size();
        available.notify_one();
        return true;
    }

    bool SubmitDamage(LoggedDamage command) {
        std::scoped_lock lock(mutex);
        if (!status.running || stopping) return false;
        if (queued_events >= kMaximumQueuedEvents) {
            ++status.dropped_events;
            if (status.dropped_first_sequence == 0) {
                status.dropped_first_sequence = command.event.sequence;
            }
            status.dropped_last_sequence = command.event.sequence;
            return false;
        }
        commands.emplace_back(std::move(command));
        ++queued_events;
        status.queue_depth = commands.size();
        available.notify_one();
        return true;
    }

    void Run() {
        DamageLogRepository repository(root);
        std::string error;
        const bool recovered = repository.Recover(error);
        {
            std::scoped_lock lock(mutex);
            status.ready = recovered;
            status.last_error = error;
            status.history = repository.History();
        }
        auto last_flush = std::chrono::steady_clock::now();
        for (;;) {
            Command command;
            bool has_command{};
            {
                std::unique_lock lock(mutex);
                available.wait_for(lock, std::chrono::seconds(1), [&] {
                    return stopping || !commands.empty();
                });
                if (!commands.empty()) {
                    command = std::move(commands.front());
                    if (std::holds_alternative<LoggedDamage>(command)) --queued_events;
                    commands.pop_front();
                    status.queue_depth = commands.size();
                    has_command = true;
                } else if (stopping) {
                    break;
                }
            }
            bool succeeded = true;
            error.clear();
            bool refresh_history{};
            if (has_command) {
                std::visit([&](const auto& value) {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, ReportOpen>) {
                        succeeded = repository.BeginReport(value, error);
                        refresh_history = true;
                    } else if constexpr (std::is_same_v<Value, EncounterOpen>) {
                        succeeded = repository.BeginEncounter(value, error);
                    } else if constexpr (std::is_same_v<Value, LoggedDamage>) {
                        succeeded = repository.Append(value, error);
                    } else if constexpr (std::is_same_v<Value, EncounterClose>) {
                        succeeded = repository.EndEncounter(value, error);
                        refresh_history = true;
                    } else if constexpr (std::is_same_v<Value, ReportClose>) {
                        succeeded = repository.CloseReport(value, error);
                        refresh_history = true;
                    } else if constexpr (std::is_same_v<Value, DeleteCommand>) {
                        succeeded = repository.DeleteReport(value.report_id, error);
                        refresh_history = true;
                    }
                }, command);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= std::chrono::seconds(1)) {
                std::string flush_error;
                if (!repository.Flush(flush_error)) {
                    succeeded = false;
                    if (error.empty()) error = std::move(flush_error);
                }
                refresh_history = true;
                last_flush = now;
            }
            std::scoped_lock lock(mutex);
            if (!succeeded) status.last_error = error.empty() ? "log write failed" : error;
            if (refresh_history) status.history = repository.History();
        }
        std::string flush_error;
        if (!repository.Flush(flush_error)) {
            std::scoped_lock lock(mutex);
            status.last_error = std::move(flush_error);
        }
        std::scoped_lock lock(mutex);
        status.history = repository.History();
        status.ready = false;
    }
};

DamageLogWorker::DamageLogWorker(std::filesystem::path root)
    : impl_(std::make_unique<Impl>(std::move(root))) {}

DamageLogWorker::~DamageLogWorker() {
    Stop();
}

bool DamageLogWorker::Start() {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->status.running) return true;
    impl_->stopping = false;
    impl_->status = {};
    impl_->status.running = true;
    try {
        impl_->thread = std::thread([this] { impl_->Run(); });
    } catch (...) {
        impl_->status.running = false;
        impl_->status.last_error = "failed to create log writer thread";
        return false;
    }
    return true;
}

void DamageLogWorker::Stop() {
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->status.running) return;
        impl_->stopping = true;
        impl_->available.notify_one();
    }
    if (impl_->thread.joinable()) impl_->thread.join();
    std::scoped_lock lock(impl_->mutex);
    impl_->status.running = false;
    impl_->status.queue_depth = 0;
    impl_->commands.clear();
    impl_->queued_events = 0;
}

bool DamageLogWorker::Submit(const ReportOpen& command) {
    return impl_->SubmitControl(command);
}
bool DamageLogWorker::Submit(const EncounterOpen& command) {
    return impl_->SubmitControl(command);
}
bool DamageLogWorker::Submit(const LoggedDamage& command) {
    return impl_->SubmitDamage(command);
}
bool DamageLogWorker::Submit(const EncounterClose& command) {
    return impl_->SubmitControl(command);
}
bool DamageLogWorker::Submit(const ReportClose& command) {
    return impl_->SubmitControl(command);
}
bool DamageLogWorker::DeleteReport(std::string report_id) {
    return impl_->SubmitControl(DeleteCommand{std::move(report_id)});
}

LogWorkerStatus DamageLogWorker::Status() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->status;
}

std::filesystem::path DefaultLogRoot() {
    PWSTR local_app_data{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
            KF_FLAG_CREATE, nullptr, &local_app_data))) return {};
    const std::filesystem::path root = std::filesystem::path(local_app_data) /
        "NTECombatStats" / "logs" / "v1";
    CoTaskMemFree(local_app_data);
    return root;
}

std::string GenerateUuidV4() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (std::size_t index{}; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) value << '-';
        value << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return value.str();
}

}  // namespace nte::damage_log
