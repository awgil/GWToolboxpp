#pragma once

#include <chrono>

class TreasureRenderer {
public:
    struct Treasure {
        GW::Vec2f pos;
    };
    using TreasureList = std::vector<Treasure>;

    struct PendingTreasure : Treasure {
        std::chrono::system_clock::time_point lastPing;
    };

    struct PendingScan {
        GW::Vec2f pos;
        std::chrono::system_clock::time_point expire;
    };

    void Initialize();
    void Terminate();

    void LoadSettings(ToolboxIni* ini, const char* section);
    void SaveSettings(ToolboxIni* ini, const char* section) const;

    void Update();
    void Render(IDirect3DDevice9* device);

private:
    void OnMapChanged();
    void OnTreasurePing(GW::Vec2f pos, bool found);

private:
    std::unordered_map<GW::Constants::MapID, TreasureList> mKnownTreasures;
    std::vector<PendingTreasure> mExpectedTreasures;
    std::optional<PendingScan> mPendingScan;
    IDirect3DVertexBuffer9* mCircleVB = nullptr;
};
