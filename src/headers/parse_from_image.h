#ifndef PARSE_FROM_IMAGE_H
#define PARSE_FROM_IMAGE_H

#include <string>
#include <fstream>
#include <utility>

/**
 * Get the size of a file in bytes.
 *
 * @param filename Path to the file
 * @return Size of the file in bytes
 */
std::ifstream::pos_type filesize(const char* filename);

/**
 * Parse data from an image created by parseToImage.
 * This function can handle both single images and multi-part image sets.
 * When given any image from a multi-part set, it will automatically locate and process all parts.
 *
 * The image header is expected to have the following format:
 *   <output filename>#<10-digit data size>#<index info>#
 * where the index info is in the format "XXXX-YYYY" for multi-part images.
 *
 * @param filename Path to the image file (any part if split across multiple images)
 */
void parseFromImage(const std::string& filename);

#endif // PARSE_FROM_IMAGE_H
