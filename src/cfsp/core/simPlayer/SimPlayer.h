#pragma once

#include "SimPlayerSaveData.h"
#include "cfsp/base/Macros.h"
#include "cfsp/base/OperateResult.h"
#include "cfsp/core/simPlayer/SimPlayerPermission.h"
#include "cfsp/core/simPlayer/SpDriver.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/server/ServerPlayer.h"
#include <memory>
#include <optional>


namespace coral_fans::cfsp::simulated_player {
class SimPlayer : public std::enable_shared_from_this<SimPlayer> {
public:
    SimPlayerSaveData mSaveData;
    // The player in the world. Null while offline and while the login is still in
    // progress (see mConnecting).
    ServerPlayer*      mSimPlayer  = nullptr;
    bool               mConnecting = false;
    unsigned long long mTaskid     = 0;
    unsigned long long mScriptid   = 0;
    bool               mShouldSave = true;

public:
    SimPlayer(SimPlayerSaveData saveData, ServerPlayer* sp = nullptr);

public:
    bool save();
    bool loadSpNbt();
    // Takes the player out of the world without saving anything.
    void disconnect();
    // The player went away behind our back; forget it. player is non-null while it
    // is still valid and can be saved one last time.
    void onLost(ServerPlayer* player);
    // The server is shutting down and removes the player itself; just let go of it.
    void onServerStopping();

private:
    void forgetUnlockedIdentity();
    void onCreated(ServerPlayer& sp, Vec3 const& pos, DimensionType dim, bool lockUniqueId, Vec3 const& lookDir);
    void onSpawned(ServerPlayer& sp);

public:
    bool checkInvEmptyForOfflineCFSP();

public:
    CFSP_API std::string getName();
    CFSP_API std::string getXuid();
    CFSP_API bool        isOnline();
    CFSP_API bool        isFree();

public:
    bool                hasPermission(const Player*, SimPlayerPermission);
    uint                getPermission(Player*);
    base::OperateResult setPermission(Player*, SimPlayerPermission);

public:
    enum class Direction : int { North, South, West, East, Up, Down };

public:
    CFSP_API void cancelTask();
    CFSP_API void cancelScript();
    CFSP_API base::OperateResult stop();
    CFSP_API static std::shared_ptr<SimPlayer>
    create(const Player* player, std::string const& name, Vec3 const& pos, DimensionType dim, bool lockUniqueId = true);
    CFSP_API base::OperateResult spawn(std::optional<const Player*> player); // 当player为std::nullopt时，为autojoin
    CFSP_API base::OperateResult despawn();
    CFSP_API base::OperateResult respawn();
    CFSP_API base::OperateResult lookAt(Vec3 const& pos);
    CFSP_API base::OperateResult lookAt(Direction direction);
    CFSP_API base::OperateResult info();

public:
    CFSP_API bool isEmptyInv();
    CFSP_API base::OperateResult invInfo();
    CFSP_API base::OperateResult drop(int times = 1, int interval = 1);
    CFSP_API base::OperateResult dropInv(int times = 1, int interval = 1);
    CFSP_API base::OperateResult swap(Player*);
    CFSP_API base::OperateResult select(int id);
    CFSP_API base::OperateResult openInv(Player*);

public:
    CFSP_API base::OperateResult sneaking(std::optional<bool> enable = std::nullopt);
    CFSP_API base::OperateResult swimming(std::optional<bool> enable = std::nullopt);
    CFSP_API base::OperateResult flying(std::optional<bool> enable = std::nullopt);
    CFSP_API base::OperateResult sprinting(std::optional<bool> enable = std::nullopt);

public:
    CFSP_API base::OperateResult attack(int times = 1, int interval = 1);
    CFSP_API base::OperateResult build(int times = 1, int interval = 1);
    CFSP_API base::OperateResult interact(int times = 1, int interval = 1);
    CFSP_API base::OperateResult jump(int times = 1, int interval = 1);
    CFSP_API base::OperateResult use(int _long = 10, int times = 1, int interval = 1);
    CFSP_API base::OperateResult destroy(int _long = 1, int times = 1, int interval = 1);

public:
    CFSP_API base::OperateResult chat(std::string const&);
    CFSP_API base::OperateResult runcmd(std::string const&);

public:
    CFSP_API base::OperateResult moveTo(Vec3 const& pos, float speed = 4.3f);
    CFSP_API base::OperateResult navTo(Vec3 const& pos, float speed = 4.3f);
    CFSP_API base::OperateResult tp(Vec3 pos, std::optional<int> dimId = std::nullopt);
};
} // namespace coral_fans::cfsp::simulated_player