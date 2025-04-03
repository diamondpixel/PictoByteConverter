#include "headers/parse_to_image.h"
#include "headers/parse_from_image.h"
#include <iostream>

using namespace std;

int main() {
    parseToImage("D:\\EnchantedWaterfall.wav", "D:\\EnchantedWaterfall.bmp", 100);
    parseFromImage("D:\\EnchantedWaterfall.bmp");
    return 0;
}
