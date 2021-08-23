#ifndef INFER_CONTROLLER_HPP
#define INFER_CONTROLLER_HPP

#include <string>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <infer/trt_infer.hpp>
#include "monopoly_allocator.hpp"

template<class Input, class Output, class StartParam=std::tuple<std::string, int>, class JobAdditional=int>
class InferController{
public:
    struct Job{
        Input input;
        Output output;
        JobAdditional additional;
        MonopolyAllocator<TRT::Tensor>::MonopolyDataPointer mono_tensor;
        std::shared_ptr<std::promise<Output>> pro;
    };

    virtual ~InferController(){
        stop();
    }

    void stop(){
        run_ = false;
        cond_.notify_all();

        if(worker_){
            worker_->join();
            worker_.reset();
        }
    }

    bool startup(const StartParam& param){
        run_ = true;

        std::promise<bool> pro;
        start_param_ = param;
        worker_      = std::make_shared<std::thread>(&InferController::worker, this, std::ref(pro));
        return pro.get_future().get();
    }

    virtual std::shared_future<Output> commit(const Input& input){

        Job job;
        job.pro = std::make_shared<std::promise<Output>>();
        if(!preprocess(job, input)){
            job.pro->set_value(Output());
            return job.pro->get_future();
        }
        
        ///////////////////////////////////////////////////////////
        {
            std::unique_lock<std::mutex> l(jobs_lock_);
            jobs_.push(job);
        };
        cond_.notify_one();
        return job.pro->get_future();
    }

protected:
    virtual void worker(std::promise<bool>& result) = 0;
    virtual bool preprocess(Job& job, const Input& input) = 0;
    
    virtual bool get_jobs_and_wait(std::vector<Job>& fetch_jobs, int max_size){

        std::unique_lock<std::mutex> l(jobs_lock_);
        cond_.wait(l, [&](){
            return !run_ || !jobs_.empty();
        });

        if(!run_) return false;
        
        for(int i = 0; i < max_size && !jobs_.empty(); ++i){
            fetch_jobs.emplace_back(std::move(jobs_.front()));
            jobs_.pop();
        }
        return true;
    }

    virtual bool get_job_and_wait(Job& fetch_job){

        std::unique_lock<std::mutex> l(jobs_lock_);
        cond_.wait(l, [&](){
            return !run_ || !jobs_.empty();
        });

        if(!run_) false;
        
        fetch_job = std::move(jobs_.front());
        jobs_.pop();
        return true;
    }

protected:
    StartParam start_param_;
    std::atomic<bool> run_;
    std::mutex jobs_lock_;
    std::queue<Job> jobs_;
    std::shared_ptr<std::thread> worker_;
    std::condition_variable cond_;
    std::shared_ptr<MonopolyAllocator<TRT::Tensor>> tensor_allocator_;
};

#endif // INFER_CONTROLLER_HPP