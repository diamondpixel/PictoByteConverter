#include "headers/ThreadSafeQueue.h"
#include "../Tasks/headers/ImageTaskInternal.h"
#include "../Tasks/headers/_Task.h"
#include <utility>
#include "../Debug/headers/LogBufferManager.h"
#include "../Debug/headers/LogBuffer.h"
#include "../Threading/headers/ResourceManager.h"
#include <memory>

using RM = ResourceManager;

// Implementation of ThreadSafeQueue methods
#include "inlines/ThreadSafeQueue.inl"

// Explicit template instantiation for unique_ptr tasks
template class ThreadSafeQueue<std::unique_ptr<Task>>;
