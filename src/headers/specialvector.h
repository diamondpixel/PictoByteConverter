#ifndef H_SPECVEC
#define H_SPECVEC

#include <vector>

template <typename T>
class VectorWrapper : public std::vector<T> {
public:
    struct triplet {
        T e0, e1, e2;
    };

    // Use references for better performance
    triplet get_triplet(int index, const T& default_val) const;
    const T& get_element(int index, const T& default_val) const;
};

template<typename T>
typename VectorWrapper<T>::triplet VectorWrapper<T>::get_triplet(int index, const T& default_val) const {
    // Calculate the base index once
    const int base_index = index * 3;

    // Return a new triplet with all three elements
    return {
        get_element(base_index, default_val),
        get_element(base_index + 1, default_val),
        get_element(base_index + 2, default_val)
    };
}

template<typename T>
const T& VectorWrapper<T>::get_element(int index, const T& default_val) const {
    // Use size_type for index comparisons
    return (index >= 0 && static_cast<typename std::vector<T>::size_type>(index) < this->size())
        ? this->operator[](index)
        : default_val;
}

// Template instantiations
template class VectorWrapper<unsigned char>;
template class VectorWrapper<char>;
template class VectorWrapper<int>;

#endif