#include "SimPlayer.h"
#include "cfsp/CFSP.h"
#include "cfsp/base/Utils.h"
#include "ll/api/Config.h"
#include "mc/dataloadhelper/DefaultDataLoadHelper.h"
#include "mc/deps/nbt/CompoundTag.h"
#include <filesystem>

namespace coral_fans::cfsp::simulated_player {

// Writes data.json only, which never touches the player, so it is safe even after the
// server's main loop has ended. readGameType is off there because reading anything off
// a player that is being torn down is not worth the risk.
// Where this player's data.json / nbt live. mDataDir is pinned when the player comes
// online, so that a save while the world is closing still reaches that world.
std::filesystem::path SimPlayer::dataDir() const {
    if (!this->mDataDir.empty()) return this->mDataDir;
    return base::utils::getSimplayerDataDir();
}

bool SimPlayer::saveData(bool readGameType) {
    auto dataDir = this->dataDir();
    if (readGameType && this->mSimPlayer && this->mSaveData.mGameType != this->mSimPlayer->getPlayerGameType()) {
        this->mSaveData.mGameType = this->mSimPlayer->getPlayerGameType();
        this->mShouldSave         = true;
    }
    if (!this->mShouldSave) [[likely]]
        return true;
    if (ll::config::saveConfig(
            this->mSaveData,
            dataDir / reinterpret_cast<const char8_t*>(this->mSaveData.name.c_str()) / "data.json"
        )) [[likely]]
        this->mShouldSave = false;
    return !this->mShouldSave;
}

bool SimPlayer::save() {
    auto dataDir = this->dataDir();
    if (!this->saveData()) [[unlikely]]
        return false;
    if (!this->mSimPlayer) [[unlikely]]
        return false;
    auto tag = std::make_unique<CompoundTag>();
    if (!this->mSimPlayer->save(*tag)) [[unlikely]]
        return false;
    if (!tag) [[unlikely]]
        return false;
    std::ofstream f(
        dataDir / reinterpret_cast<const char8_t*>(this->mSaveData.name.c_str()) / "nbt",
        std::ios_base::out | std::ios_base::trunc
    );
    if (!f.is_open()) [[unlikely]]
        return false;
    f << tag->toSnbt(SnbtFormat::Minimize);
    f.close();
    return true;
}

bool SimPlayer::loadSpNbt() {
    if (!this->mSimPlayer) [[unlikely]]
        return false;
    auto dataDir = this->dataDir();
    auto path = dataDir / reinterpret_cast<const char8_t*>(this->mSaveData.name.c_str()) / "nbt";
    if (!std::filesystem::exists(path)) [[unlikely]]
        return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) [[unlikely]]
        return false;
    try {
        auto size = f.tellg();
        f.seekg(0);
        std::string content(size, '\0');
        f.read(content.data(), size);
        std::string_view      snbt{content};
        DefaultDataLoadHelper helper;
        auto                  tag = CompoundTag::fromSnbt(snbt).value();
        this->mSimPlayer->load(tag, helper);
        if (auto it = tag.mTags.find("DimensionId"); it != tag.mTags.end()) {
            int dimId = it->second.get<IntTag>().data;
            this->tp(this->mSimPlayer->getFeetPos(), dimId);
        }
    } catch (const std::exception& e) {
        coral_fans::cfsp::CFSP::getInstance().getSelf().getLogger().error(e.what());
        return false;
    }
    return true;
}

bool SimPlayer::checkInvEmptyForOfflineCFSP() {
    auto dataDir = this->dataDir();
    auto path = dataDir / reinterpret_cast<const char8_t*>(this->mSaveData.name.c_str()) / "nbt";
    if (!std::filesystem::exists(path)) [[unlikely]]
        return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) [[unlikely]]
        return false;
    try {
        auto size = f.tellg();
        f.seekg(0);
        std::string content(size, '\0');
        f.read(content.data(), size);
        std::string_view                snbt{content};
        DefaultDataLoadHelper           helper;
        auto                            tag     = CompoundTag::fromSnbt(snbt).value();
        static std::vector<std::string> invKeys = {"Armor", "EnderChestInventory", "Inventory", "Offhand"};
        for (auto& key : invKeys) {
            if (auto it = tag.mTags.find(key); it != tag.mTags.end()) {
                for (auto& invTag : it->second.get<ListTag>()) {
                    auto& _invTag = invTag.get<CompoundTag>();
                    if (auto invIt = _invTag.mTags.find("Count"); invIt != _invTag.mTags.end()) {
                        if (invIt->second.get<ByteTag>().data) {
                            return false;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        coral_fans::cfsp::CFSP::getInstance().getSelf().getLogger().error(e.what());
        return false;
    }
    return true;
}
} // namespace coral_fans::cfsp::simulated_player