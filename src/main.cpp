#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <filesystem>
#include <future>

#include "Threading/headers/ThreadPool.h"
#include "Threading/headers/ResourceManager.h"
#include "Image/headers/BitmapImage.h"

// Removed heavy ImageTaskInternal dependency; using lightweight lambdas.

int main() {
    using namespace std::chrono_literals; {
        // -------------------------------------------------------------
        // 0. Ensure required directories exist
        // -------------------------------------------------------------
        std::filesystem::create_directories("./output/");
        std::filesystem::create_directories("./output/spill/");

        // -------------------------------------------------------------
        // 1. Configure system-wide resource limits via ResourceManager
        // -------------------------------------------------------------
        auto &rm = ResourceManager::getInstance();
        size_t memory = 1024 * 1024 * 1024;
        rm.setMaxMemory(memory * 1);
        std::cout << "Max RAM in Bytes: " << rm.getMaxMemory() << std::endl;

        ThreadPool pool(7,
                        100,
                        "ImagePool",
                        QueueType::LockFree);

        // -------------------------------------------------------------
        // 3. Generate and submit a batch of image-processing tasks that
        //    intentionally exceed the 50 MB memory limit so we can watch
        //    the queue spill to disk while the pool keeps working.
        // -------------------------------------------------------------
        auto start_time = std::chrono::high_resolution_clock::now();
        constexpr int num_tasks = 10; // generate 50 images asynchronously
        std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(5700, 5700);

        // Vector to store task futures
        std::vector<std::future<void>> futures;

        for (int i = 0; i < num_tasks; ++i) {
            int w = dist(rng);
            int h = dist(rng);
            std::string filename = std::format("output/image_{}.bmp", i);

            // Submit a lightweight lambda that builds, draws and saves the image inside the worker thread.
            futures.emplace_back(
                pool.submit("build_save", 0, [w, h, filename]() {
                    BitmapImage img(w, h);
                    img.draw_smiley_face();
                    auto o = img.save(filename);
                    img.clear();
                })
            );
        }

        pool.shutdown(true); // wait for workers to finish

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "\nTotal processing time: " << duration.count() << " milliseconds\n";

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
    }

    debug::LogBufferManager::getInstance().shutdown();

    return 0;
}
