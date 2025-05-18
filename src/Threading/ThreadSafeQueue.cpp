#include "headers/ThreadSafeQueue.h"
#include "../Tasks/headers/ImageTask.h"
#include "../Tasks/headers/ImageTaskInternal.h"
#include "../Debug/headers/LogBufferManager.h"
#include "../Debug/headers/LogBuffer.h"

// Explicit template instantiation for the types we need
template class ThreadSafeQueue<ImageTask>;
template class ThreadSafeQueue<ImageTaskInternal>;
