#ifndef PIPELINE_H
#define PIPELINE_H

#include <vector>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include "ThreadPool.h"
#include "ThreadSafeQueue.h"
#include "ResourceManager.h"
#include "../../Debug/headers/Debug.h" // Include Debug.h for printDebug

/**
 * @brief Pipeline for sequential multi-stage processing
 * 
 * This class implements a pipeline pattern where data flows through
 * multiple processing stages, with each stage potentially running
 * in parallel using its own thread pool.
 * 
 * Features:
 * - Multiple processing stages with independent thread pools
 * - Integration with ResourceManager for resource tracking and management
 * - Automatic data flow between stages
 * - Support for different input/output types between stages
 * - Graceful shutdown and cleanup
 * - Exception propagation
 */
template <typename InputType, typename OutputType = InputType>
class Pipeline {
public:
    /**
     * @brief Construct a new Pipeline
     * 
     * @param name Name of the pipeline (for debugging)
     * @param queue_size Maximum size of internal queues (default: unlimited)
     */
    explicit Pipeline(const std::string& name = "Pipeline", size_t queue_size = SIZE_MAX)
        : name_(name), queue_size_(queue_size), running_(false) {
        // Log pipeline creation
        printDebug("Creating Pipeline '" + name_ + "'");
    }

    /**
     * @brief Destructor ensures proper shutdown
     */
    ~Pipeline() {
        shutdown();
        printDebug("Pipeline '" + name_ + "' destroyed");
    }

    // Disable copying
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Allow moving
    Pipeline(Pipeline&& other) noexcept = default;
    Pipeline& operator=(Pipeline&& other) noexcept = default;

    /**
     * @brief Add a processing stage to the pipeline
     * 
     * @tparam StageInput Input type for this stage
     * @tparam StageOutput Output type for this stage
     * @param processor Function to process data (takes StageInput, returns StageOutput)
     * @param num_threads Number of threads for this stage (default: 1)
     * @param stage_name Name for this stage (for debugging)
     * @return Pipeline& Reference to this pipeline for method chaining
     */
    template <typename StageInput, typename StageOutput>
    Pipeline& add_stage(std::function<StageOutput(StageInput)> processor, 
                        size_t num_threads = 1,
                        const std::string& stage_name = "") {
        // Generate a stage name if not provided
        std::string actual_stage_name = stage_name.empty() 
            ? name_ + "_Stage" + std::to_string(stages_.size() + 1) 
            : stage_name;
        
        // Create a new stage with ResourceManager integration
        auto stage = std::make_shared<Stage<StageInput, StageOutput>>(
            processor, num_threads, queue_size_, actual_stage_name
        );
        
        // Log stage creation
        printDebug("Added stage '" + actual_stage_name + "' to pipeline '" + name_ + "'");
        
        // Add the stage to our list
        stages_.push_back(stage);
        
        return *this;
    }

    /**
     * @brief Start the pipeline
     */
    void start() {
        if (running_) {
            return;
        }
        
        running_ = true;
        
        // Start all stages
        for (auto& stage : stages_) {
            stage->start();
        }
        
        printDebug("Pipeline '" + name_ + "' started");
    }

    /**
     * @brief Submit an input to the pipeline
     * 
     * @param input Input data to process
     * @return std::future<OutputType> Future for the final result
     */
    std::future<OutputType> process(const InputType& input) {
        if (!running_) {
            start();
        }
        
        // Create a promise for the final result
        auto promise = std::make_shared<std::promise<OutputType>>();
        auto future = promise->get_future();
        
        // Track memory usage for the input if possible
        auto& rm = ResourceManager::getInstance();
        size_t input_size = sizeof(InputType);
        rm.trackMemory(input_size, "Pipeline_" + name_ + "_Input");
        
        // Submit the input to the first stage
        if (!stages_.empty()) {
            auto first_stage = std::static_pointer_cast<Stage<InputType, OutputType>>(stages_[0]);
            first_stage->process(input, promise);
        } else {
            // If there are no stages, just return the input as the output
            promise->set_value(static_cast<OutputType>(input));
            rm.releaseMemory(input_size);
        }
        
        return future;
    }

    /**
     * @brief Shutdown the pipeline
     */
    void shutdown() {
        if (!running_) {
            return;
        }
        
        running_ = false;
        
        // Shutdown all stages in reverse order
        for (auto it = stages_.rbegin(); it != stages_.rend(); ++it) {
            (*it)->shutdown();
        }
        
        printDebug("Pipeline '" + name_ + "' shut down");
    }

    /**
     * @brief Wait for all current tasks to complete
     */
    void wait() {
        for (auto& stage : stages_) {
            stage->wait();
        }
    }

    /**
     * @brief Check if the pipeline is running
     * 
     * @return true if running, false otherwise
     */
    bool is_running() const {
        return running_;
    }

    /**
     * @brief Get the name of the pipeline
     * 
     * @return Pipeline name
     */
    const std::string& name() const {
        return name_;
    }

private:
    /**
     * @brief Base class for pipeline stages
     */
    class StageBase {
    public:
        virtual ~StageBase() = default;
        virtual void start() = 0;
        virtual void shutdown() = 0;
        virtual void wait() = 0;
    };

    /**
     * @brief Concrete implementation of a pipeline stage
     * 
     * @tparam StageInput Input type for this stage
     * @tparam StageOutput Output type for this stage
     */
    template <typename StageInput, typename StageOutput>
    class Stage : public StageBase {
    public:
        /**
         * @brief Construct a new Stage
         * 
         * @param processor Function to process data
         * @param num_threads Number of threads for this stage
         * @param queue_size Maximum size of the input queue
         * @param stage_name Name of this stage (for debugging)
         */
        Stage(std::function<StageOutput(StageInput)> processor, 
              size_t num_threads, size_t queue_size, const std::string& stage_name)
            : processor_(processor), 
              thread_pool_(num_threads, queue_size, stage_name),
              input_queue_(queue_size),
              running_(false),
              stage_name_(stage_name) {}

        /**
         * @brief Start the stage
         */
        void start() override {
            if (running_) {
                return;
            }
            
            running_ = true;
            
            // Start the worker thread to process inputs from the queue
            worker_thread_ = std::thread([this]() {
                auto& rm = ResourceManager::getInstance();
                rm.registerThread(stage_name_);
                
                while (running_) {
                    // Get the next input from the queue
                    std::tuple<StageInput, std::shared_ptr<std::promise<StageOutput>>> item;
                    if (!input_queue_.pop(item)) {
                        // Queue is empty and we're shutting down
                        break;
                    }
                    
                    // Process the input
                    auto& input = std::get<0>(item);
                    auto promise = std::get<1>(item);
                    
                    // Submit the processing task to the thread pool
                    thread_pool_.submit([this, input, promise]() {
                        try {
                            // Process the input
                            StageOutput output = processor_(input);
                            
                            // Set the result
                            promise->set_value(output);
                        } catch (...) {
                            // Propagate any exceptions
                            promise->set_exception(std::current_exception());
                        }
                    });
                }
                
                rm.unregisterThread();
            });
        }

        /**
         * @brief Process an input and set the result in the promise
         * 
         * @param input Input to process
         * @param promise Promise to set the result in
         */
        void process(const StageInput& input, std::shared_ptr<std::promise<StageOutput>> promise) {
            if (!running_) {
                start();
            }
            
            // Push the input to the queue
            input_queue_.push(std::make_tuple(input, promise));
        }

        /**
         * @brief Shutdown the stage
         */
        void shutdown() override {
            if (!running_) {
                return;
            }
            
            running_ = false;
            
            // Signal the input queue to finish
            input_queue_.done();
            
            // Shutdown the thread pool
            thread_pool_.shutdown();
            
            // Wait for the worker thread to finish
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }

        /**
         * @brief Wait for all tasks to complete
         */
        void wait() override {
            thread_pool_.wait_for_tasks();
        }

    private:
        std::function<StageOutput(StageInput)> processor_;
        ThreadPool thread_pool_;
        ThreadSafeQueue<std::tuple<StageInput, std::shared_ptr<std::promise<StageOutput>>>> input_queue_;
        std::thread worker_thread_;
        std::atomic<bool> running_;
        std::string stage_name_;
    };

    std::string name_;
    size_t queue_size_;
    std::atomic<bool> running_;
    std::vector<std::shared_ptr<StageBase>> stages_;
};

#endif // PIPELINE_H
