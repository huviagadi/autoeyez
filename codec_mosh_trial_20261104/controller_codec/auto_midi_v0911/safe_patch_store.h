#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace autoeyez {
enum class SaveResult {
    Saved, RecoveryFailed, TempFailed, WriteFailed, VerifyFailed,
    BackupFailed, CommitFailed, RestoreFailed
};

inline bool appendPatchLine(char* data, size_t capacity, size_t& used,
                            const char* key, int value) {
    if (used >= capacity) return false;
    const int count = std::snprintf(data + used, capacity - used, "%s=%d\r\n", key, value);
    if (count < 0 || static_cast<size_t>(count) >= capacity - used) return false;
    used += static_cast<size_t>(count);
    return true;
}

template <class Storage>
bool verifyPatch(Storage& storage, const char* path, const char* data, size_t length) {
    auto file = storage.open(path);
    if (!file) return false;
    if (file.size() != length) { file.close(); return false; }
    uint8_t buffer[128];
    size_t offset = 0;
    while (offset < length) {
        const size_t count = length - offset < sizeof(buffer) ? length - offset : sizeof(buffer);
        const int read = file.read(buffer, count);
        if (read != static_cast<int>(count) || std::memcmp(buffer, data + offset, count) != 0) {
            file.close();
            return false;
        }
        offset += count;
    }
    file.close();
    return true;
}

template <class Storage>
bool recoverPatch(Storage& storage, const char* finalPath, const char* backupPath) {
    // A cut between the two renames leaves the previous patch in .BAK.
    // Never promote an uncommitted .TMP: it may contain a partial write.
    if (!storage.exists(finalPath) && storage.exists(backupPath))
        return storage.rename(backupPath, finalPath);
    return true;
}

template <class Storage>
SaveResult saveVerifiedPatch(Storage& storage, const char* finalPath,
                             const char* tempPath, const char* backupPath,
                             const char* data, size_t length, uint8_t writeMode) {
    if (!recoverPatch(storage, finalPath, backupPath)) return SaveResult::RecoveryFailed;
    if (storage.exists(tempPath) && !storage.remove(tempPath)) return SaveResult::TempFailed;
    auto file = storage.open(tempPath, writeMode);
    if (!file) return SaveResult::TempFailed;
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(data), length);
    file.flush();
    file.close();
    if (written != length) return SaveResult::WriteFailed;
    if (!verifyPatch(storage, tempPath, data, length)) return SaveResult::VerifyFailed;

    const bool hadPrevious = storage.exists(finalPath);
    if (hadPrevious) {
        // Keep one previous generation. The current .TXT still exists while
        // removing an older backup, and the new .TMP is already verified.
        if (storage.exists(backupPath) && !storage.remove(backupPath)) return SaveResult::BackupFailed;
        if (!storage.rename(finalPath, backupPath)) return SaveResult::BackupFailed;
    }
    if (!storage.rename(tempPath, finalPath)) {
        if (hadPrevious && !recoverPatch(storage, finalPath, backupPath))
            return SaveResult::RestoreFailed;
        return SaveResult::CommitFailed;
    }
    if (!verifyPatch(storage, finalPath, data, length)) {
        if (!storage.remove(finalPath)) return SaveResult::RestoreFailed;
        if (hadPrevious && !recoverPatch(storage, finalPath, backupPath))
            return SaveResult::RestoreFailed;
        return SaveResult::VerifyFailed;
    }
    // Keep .BAK after a successful save for recovery/manual rollback.
    return SaveResult::Saved;
}
} // namespace autoeyez
