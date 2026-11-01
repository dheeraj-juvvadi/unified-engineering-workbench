// job_manager.h - background jobs (currently only the benchmark).
//
// Rules this class enforces:
//   * ids are "j1", "j2", ...
//   * only one job of a given kind runs at a time (second start -> BusyError)
//   * a job runs on its own std::thread; the destructor / shutdown() cancels
//     and joins every thread, so nothing outlives the server
//   * the result JSON is published exactly once, under the mutex, and only
//     when the job finished normally - a reader never sees a partial result
//   * cancellation is cooperative: we set a std::atomic<bool> which the job
//     function hands to the core code
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <json.hpp>        // nlohmann/json, vendored in third_party/

namespace wb_srv {

using json = nlohmann::json;

enum class JobState { Running, Done, Cancelled, Error };
const char* to_string(JobState s);

// Thrown by start() when a job of the same kind is already running.
struct BusyError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// What a job function sees: the cancel flag and ways to report progress.
class JobControl {
public:
    const std::atomic<bool>& cancel() const { return cancel_; }
    void set_progress(double fraction);                 // clamped to [0,1]
    void set_message(const std::string& m);
private:
    friend class JobManager;
    std::atomic<bool> cancel_{false};
    std::atomic<double> progress_{0.0};
    mutable std::mutex msg_mu_;
    std::string message_;
};

// Immutable copy of a job's state, safe to read without any lock.
struct JobSnapshot {
    std::string id, kind;
    JobState state = JobState::Running;
    double progress = 0;
    std::string message;
    std::shared_ptr<const json> result;   // null unless state == Done
    std::string error;                    // only for state == Error
};

class JobManager {
public:
    using JobFn = std::function<json(JobControl&)>;

    JobManager() = default;
    ~JobManager() { shutdown(); }
    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    // Starts `fn` on a new thread and returns the job id. Throws BusyError if
    // a job of `kind` is already running.
    std::string start(const std::string& kind, JobFn fn);
    bool snapshot(const std::string& id, JobSnapshot& out) const;
    // False if the id is unknown. Cancelling a finished job is a harmless no-op.
    bool cancel(const std::string& id);
    bool any_running() const;
    // Cancel everything and join all threads. Safe to call twice.
    void shutdown();

private:
    struct Job {
        std::string id, kind;
        JobControl ctl;
        JobState state = JobState::Running;     // guarded by mu_
        std::shared_ptr<const json> result;     // guarded by mu_
        std::string error;                      // guarded by mu_
        std::atomic<bool> finished{false};      // set last by the worker thread
        std::thread th;
    };
    void run_job(Job& job, JobFn fn);
    void reap_finished_locked();                // join threads that already ended

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    unsigned next_id_ = 1;
    bool shut_down_ = false;
};

}  // namespace wb_srv
