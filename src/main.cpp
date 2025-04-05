#include "headers/parse_to_image.h"
#include "headers/parse_from_image.h"

using namespace std;

int main() {
    int i = 01;

    switch (i) {
        case 0:
            parseToImage("D:\\EnchantedWaterfall.wav", "D:\\EnchantedWaterfall.bmp", 9);
            break;
        case 1:
            parseFromImage("D:\\EnchantedWaterfall.bmp");
            break;
        default:
            return 1;
    }

    return 0;
}
