#ifndef PARSE_TO_IMAGE_H
#define PARSE_TO_IMAGE_H

#include <string>

/**
 * Converts a file to one or more bitmap images.
 * The function embeds the original file data into the RGB values of the pixels.
 * Large files will be split across multiple images if they exceed the size limit.
 *
 * @param filename Path to the input file to be converted
 * @param outfile Path for the output bitmap image(s)
 * @param max_size_mb Maximum size of each output image in megabytes (default: 100)
 *        The output images will never exceed this size limit
 */
void parseToImage(const std::string& filename, const std::string& outfile, size_t max_size_mb = 100);

#endif // PARSE_TO_IMAGE_H