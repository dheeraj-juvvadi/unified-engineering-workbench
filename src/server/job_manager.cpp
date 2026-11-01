// job_manager.cpp - see job_manager.h for the rules.
#include "job_manager.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace wb_srv {

const char* to_string(JobState s) {
    switch (s) {
        case JobState::Running:   return "running";
        case JobState::Done:      return "done";
        case JobState::Cancelled: return "cancelled";
        case JobState::Error:     return "error";
    }
    return "error";
}

void JobControl::set_progress(double f) {
    if (!(f >= 0.0)) f = 0.0;            // also catches NaN
    if (f > 1.0) f = 1.0;
    progress_.store(f, std::memory_order_relaxed);
}

void JobControl::set_message(const std::string& m) {
    std::lock_guard<std::mutex> lk(msg_mu_);
    message_ = m;
}

std::string JobManager::start(const std::string& kind, JobFn fn) {
    std::lock_guard<std::mutex> lk(mu_);
    if (shut_down_) throw BusyError("server is shutting down");
    reap_finished_locked();
    for (auto& kv : jobs_)
        if (kv.second->kind == kind && !kv.second->finished.load())
            throw BusyError("a " + kind + " job is already running (" + kv.first + ")");

    auto job = std::make_shared<Job>();
    job->id = "j" + std::to_string(next_id_++);
    job->kind = kind;
    jobs_[job->id] = job;
    // The thread keeps the Job alive through the shared_ptr copy.
    job->th = std::thread([this, job, fn = std::move(fn)]() mutable { run_job(*job, std::move(fn)); });
    return job->id;
}

void JobManager::run_job(Job& job, JobFn fn) {
    JobState end_state = JobState::Done;
    std::shared_ptr<const json> result;
    std::string err;
    try {
        json r = fn(job.ctl);
        if (job.ctl.cancel_.load()) end_state = JobState::Cancelled;   // result discarded
        else result = std::make_shared<const json>(std::move(r));
    } catch (const std::exception& e) {
        end_state = job.ctl.cancel_.load() ? JobState::Cancelled : JobState::Error;
        err = e.what();
    } catch (...) {
        end_state = JobState::Error;
        err = "unknown error";
    }
    {
        // One critical section: state, result and error change together.
        std::lock_guard<std::mutex> lk(mu_);
        job.state = end_state;
        job.result = end_state == JobState::Done ? std::move(result) : nullptr;
        job.error = end_state == JobState::Error ? err : std::string();
        if (end_state == JobState::Done) job.ctl.progress_.store(1.0);
    }
    job.finished.store(true);
}

bool JobManager::snapshot(const std::string& id, JobSnapshot& out) const {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        job = it->second;
        out.id = job->id;
        out.kind = job->kind;
        out.state = job->state;
        out.result = job->result;
        out.error = job->error;
    }
    out.progress = job->ctl.progress_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(job->ctl.msg_mu_);
        out.message = job->ctl.message_;
    }
    return true;
}

bool JobManager::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return false;
    if (it->second->state == JobState::Running) it->second->ctl.cancel_.store(true);
    return true;
}

bool JobManager::any_running() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : jobs_)
        if (!kv.second->finished.load()) return true;
    return false;
}

void JobManager::reap_finished_locked() {
    for (auto& kv : jobs_) {
        Job& j = *kv.second;
        if (j.finished.load() && j.th.joinable()) j.th.join();   // returns at once
    }
}

void JobManager::shutdown() {
    std::vector<std::shared_ptr<Job>> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        shut_down_ = true;
        for (auto& kv : jobs_) {
            kv.second->ctl.cancel_.store(true);
            all.push_back(kv.second);
        }
    }
    // Join outside the lock: run_job() needs mu_ to publish its final state.
    for (auto& j : all)
        if (j->th.joinable()) j->th.join();
}

}  // namespace wb_srv
