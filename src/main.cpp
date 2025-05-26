#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <filesystem>
#include <future>
#include <Debug/headers/Debug.h>

#include "Threading/headers/ThreadPool.h"
#include "Threading/headers/ResourceManager.h"
#include "Image/headers/BitmapImage.h"
#include "Tasks/headers/ImageTaskInternal.h"

// Function to process an image task
void processImageTask(ImageTaskInternal &task) {
    // Persist the bitmap to disk (BMP)
   task.image.save(task.filename);
    // Clear memory after use
    task.image.clear();
}

int main() {
    using namespace std::chrono_literals;

    // -------------------------------------------------------------
    // 0. Ensure required directories exist
    // -------------------------------------------------------------
    std::filesystem::create_directories("./output");
    std::filesystem::create_directories("./spill");

    // -------------------------------------------------------------
    // 1. Configure system-wide resource limits via ResourceManager
    // -------------------------------------------------------------
    auto &rm = ResourceManager::getInstance();
    rm.setMaxMemory(64 * 1024 * 1024);
    std::cout << "Max RAM in Bytes: " << rm.getMaxMemory() << std::endl;
    gDebugMode = true;

    // -------------------------------------------------------------
    // 2. Create a ThreadPool that uses a SpillableQueue<Task>
    //    All tasks submitted to this pool will therefore be subject
    //    to automatic in-memory vs. on-disk management.
    // -------------------------------------------------------------
    ThreadPool pool(/*threads*/1,
                               /*queue_size (unused for Spillable)*/100,
                               /*pool name*/"ImagePool",
                               /*queue type*/QueueType::Spillable,
                               /*spill dir*/"./spill");

    // -------------------------------------------------------------
    // 3. Generate and submit a batch of image-processing tasks that
    //    intentionally exceed the 50 MB memory limit so we can watch
    //    the queue spill to disk while the pool keeps working.
    // -------------------------------------------------------------
    const int num_tasks = 50;
    std::mt19937 rng{std::random_device{}()};
    //std::uniform_int_distribution<int> dist(5700, 5700);
    std::uniform_int_distribution<int> dist(2700, 2700);

    // Vector to store all the futures
    std::vector<std::future<void> > futures;

    for (int i = 0; i < num_tasks; ++i) {
        // Create a moderately large RGB bitmap filled with random bytes
        int w = dist(rng);
        int h = dist(rng);
        BitmapImage img(w, h);
        img.draw_smiley_face();

        std::string filename = std::format("output/image_{}.bmp", i);
        uint64_t task_id = rm.getNextTaskId();
        std::string task_id_str = std::to_string(task_id);

        // 1. Build the image task on the heap
        auto taskPtr = std::make_unique<ImageTaskInternal>(
                          filename,
                          std::move(img),
                          std::to_string(task_id));

        // 2. Attach the processing callable *inside the object*
        taskPtr->setFunction([raw = taskPtr.get()] {
                processImageTask(*raw);
        });

        // 3. Submit the whole object – all properties preserved
        std::future<void> fut = pool.submit(std::move(taskPtr));
        futures.push_back(std::move(fut));
    }

    pool.shutdown(true);

    // -------------------------------------------------------------
    // 4. Process the results of all tasks
    // -------------------------------------------------------------
    std::cout << "\nChecking task completion status:\n";
    for (size_t i = 0; i < futures.size(); ++i) {
        try {
            // Wait for the task to complete and get its result
            futures[i].get();
            std::cout << "Task " << i << ": Completed successfully\n";
        } catch (const std::exception &e) {
            // If an exception was thrown during task execution
            std::cerr << "Task " << i << ": Failed with exception - " << e.what() << std::endl;
        }
    }

    return 0;
}
