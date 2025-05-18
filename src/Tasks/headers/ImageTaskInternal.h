#ifndef IMAGE_TASK_INTERNAL_H
#define IMAGE_TASK_INTERNAL_H

#include <string>
#include <fstream>
#include "../../Image/headers/BitmapImage.h"
#include "../../Debug/headers/Debug.h" // For printError, printWarning

/**
 * ImageTaskInternal struct
 * 
 * Represents an internal image processing task with serialization capabilities.
 * Contains a filename and a BitmapImage.
 */
struct ImageTaskInternal {
    std::string filename;
    BitmapImage image;

    ImageTaskInternal() = default;

    ImageTaskInternal(std::string fname, BitmapImage img)
        : filename(std::move(fname)), image(std::move(img)) {
    }

    bool serialize(std::ostream &os) const {
        if (!os.good()) return false;
        size_t filename_len = filename.length();
        os.write(reinterpret_cast<const char *>(&filename_len), sizeof(filename_len));
        if (filename_len > 0) {
            os.write(filename.c_str(), static_cast<std::streamsize>(filename_len));
        }
        if (!os.good()) {
            printError("ImageTaskInternal::serialize: Failed to write filename.");
            return false;
        }
        return image.serialize(os);
    }

    bool deserialize(std::istream &is) {
        if (!is.good()) return false;
        size_t filename_len = 0;
        is.read(reinterpret_cast<char *>(&filename_len), sizeof(filename_len));
        if (is.fail() || filename_len > 1024) {
            // Sanity check on filename length
            printError(
                "ImageTaskInternal::deserialize: Filename length read from stream is too large or read failed. Length: "
                + std::to_string(filename_len));
            return false;
        }
        if (filename_len > 0) {
            try {
                filename.resize(filename_len);
            } catch (const std::bad_alloc &e) {
                printError(
                    "ImageTaskInternal::deserialize: Failed to allocate memory for filename: " + std::string(e.what()));
                return false;
            }
            is.read(&filename[0], static_cast<std::streamsize>(filename_len));
            if (is.fail()) {
                printError("ImageTaskInternal::deserialize: Failed to read filename data.");
                return false;
            }
        } else {
            filename.clear();
        }
        return image.deserialize(is);
    }
};

#endif // IMAGE_TASK_INTERNAL_H
