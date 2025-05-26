#include "headers/SpillableQueue.h"
#include "../Tasks/headers/ImageTaskInternal.h"
#include "../Tasks/headers/_Task.h"
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <functional>
#include <deque>
#include <utility>
#include "../Debug/headers/LogBufferManager.h"
#include "../Threading/headers/ResourceManager.h"

using RM = ResourceManager;

// Implementation of SpillableQueue methods
#include "inlines/SpillableQueue.inl"

// Explicit template instantiations
template class SpillableQueue<ImageTaskInternal>;
template class SpillableQueue<Task>;
