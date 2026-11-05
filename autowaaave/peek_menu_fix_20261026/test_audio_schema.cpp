#include "audio_patch_schema.h"
#include <cassert>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1]);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    assert(file.good() || file.eof());
    const std::string oldText = buffer.str();
    autoeyez::AudioPatch oldPatch;
    assert(autoeyez::readAudioPatch(oldText, 29, oldPatch));
    for (int band = 0; band < 3; ++band)
        assert(oldPatch.coefficients[band][26] == 1.0f);

    std::string next = oldText;
    const auto version = next.find("audio_version=2");
    assert(version != std::string::npos);
    next.replace(version, sizeof("audio_version=2") - 1, "audio_version=3");
    next += "low27=0.3\nmid27=0.6\nhigh27=0.9\n";
    autoeyez::AudioPatch modern;
    assert(autoeyez::readAudioPatch(next, 29, modern));
    assert(modern.coefficients[0][26] > 0.29f && modern.coefficients[2][26] > 0.89f);
}
