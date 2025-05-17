#include "headers/SpillableQueue.h"
#include "../Tasks/headers/ImageTaskInternal.h"

// Explicit template instantiation for the types we need
template class SpillableQueue<ImageTaskInternal>;
