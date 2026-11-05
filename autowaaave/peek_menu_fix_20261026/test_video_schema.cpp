#include "video_patch_schema.h"
#include <cassert>
#include <cstring>

int main() {
    autoeyez::VideoPatch modern;
    for (const auto& field : autoeyez::patchFields) {
        int value = field.low > 0 ? field.low : 0;
        if (std::strcmp(field.key, "patchVersion") == 0) value = 5;
        if (std::strcmp(field.key, "peek") == 0) value = 57;
        if (std::strcmp(field.key, "peekMask") == 0) value = 1;
        assert(modern.put(field.key, value));
    }
    assert(modern.finish(0));
    assert(modern.get("peek") == 57 && modern.get("peekMask") == 1);

    autoeyez::VideoPatch old;
    for (const auto& field : autoeyez::patchFields) {
        if (std::strcmp(field.key, "peek") == 0 ||
            std::strcmp(field.key, "peekMask") == 0) continue;
        int value = field.low > 0 ? field.low : 0;
        if (std::strcmp(field.key, "patchVersion") == 0) value = 4;
        assert(old.put(field.key, value));
    }
    assert(old.finish(0));
    assert(old.get("peek") == 0 && old.get("peekMask") == 0);

    autoeyez::VideoPatch incomplete;
    assert(incomplete.put("patchVersion", 5));
    assert(incomplete.put("slot", 0));
    assert(!incomplete.finish(0));
}
