#include "stdafx.h"

#include <GWCA/Packets/StoC.h>

#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Skill.h>

#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>

#include <Widgets/Minimap/TreasureRenderer.h>
#include <Widgets/Minimap/D3DVertex.h>

#include <Utils/GuiUtils.h>
#include <Color.h>

namespace {
    constexpr uint32_t kEffectTreasureHint = 2006;
    constexpr uint32_t kEffectTreasureFound = 1854;
    constexpr float kScanRadius = GW::Constants::Range::Compass;
    constexpr float kFindRadius = 310; // TODO: find exact; 313.8 >= x > 310.3 or so...

    using namespace std::chrono_literals;
    constexpr auto kScanExpire = 2s; // after this time passes since LD cast, we remove all treasures in scan range that were not pinged
    constexpr auto kScanThreshold = kScanExpire + 0.5s; // any pings older than this at the moment scan expires are ignored

    TreasureRenderer* gSelf = nullptr;
    GW::HookEntry gHookEntry;

    template<typename... Args> void log(std::format_string<Args...> fmt, Args&&... args)
    {
        std::print("{} [TR] ", std::chrono::system_clock::now());
        std::println(fmt, std::forward<Args>(args)...);
        std::fflush(stdout);
    }

    bool haveLightOfDeldrimor()
    {
        auto skillbar = GW::SkillbarMgr::GetPlayerSkillbar();
        if (!skillbar || !skillbar->IsValid()) {
            log("Skill bar is null!");
            return true;
        }
        return std::ranges::any_of(skillbar->skills, [](const auto& s) { return s.skill_id == GW::Constants::SkillID::Light_of_Deldrimor; });
    }
}

// serialization support
void to_json(nlohmann::json& j, const TreasureRenderer::Treasure& v)
{
    j = nlohmann::json{{"x", v.pos.x}, {"y", v.pos.y}};
}

void from_json(const nlohmann::json& j, TreasureRenderer::Treasure& v)
{
    j.at("x").get_to(v.pos.x);
    j.at("y").get_to(v.pos.y);
}

void TreasureRenderer::Initialize()
{
    ASSERT(!gSelf);
    gSelf = this;

    GW::UI::RegisterUIMessageCallback(&gHookEntry, GW::UI::UIMessage::kMapLoaded, [this](GW::HookStatus*, const GW::UI::UIMessage, void*, void*) {
        OnMapChanged();
    });
    // TODO: find the proper cast-end event rather than cast-start, theoretically LD cast time can be increased by debuffs...
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::SkillActivate>(&gHookEntry, [this](GW::HookStatus*, GW::Packet::StoC::SkillActivate* pak) {
        if (pak->skill_id != static_cast<uint32_t>(GW::Constants::SkillID::Light_of_Deldrimor)) return;
        if (pak->agent_id != GW::Agents::GetControlledCharacterId()) return;
        auto me = GW::Agents::GetControlledCharacter();
        if (!me) return;
        //log("LD cast start: [{}, {}]", me->pos.x, me->pos.y);
        mPendingScan = {me->pos, std::chrono::system_clock::now() + kScanExpire};
    });
    GW::StoC::RegisterPacketCallback<GW::Packet::StoC::PlayEffect>(&gHookEntry, [this](GW::HookStatus*, GW::Packet::StoC::PlayEffect* pak) {
        if (pak->effect_id != kEffectTreasureHint && pak->effect_id != kEffectTreasureFound) return;
        //auto me = GW::Agents::GetControlledCharacter();
        //if (!me) return;
        //log("eff: {} [{}, {}] ({}) ({} {} {} {})", pak->effect_id, pak->coords.x, pak->coords.y, GW::GetDistance(pak->coords, me->pos), pak->plane, pak->agent_id, pak->data5, pak->data6);
        OnTreasurePing(pak->coords, pak->effect_id == kEffectTreasureFound);
    });
}

void TreasureRenderer::Terminate()
{
    ASSERT(gSelf == this);
    gSelf = nullptr;

    GW::StoC::RemoveCallbacks(&gHookEntry);
    GW::UI::RemoveUIMessageCallback(&gHookEntry);

    if (mCircleVB) mCircleVB->Release();
}

void TreasureRenderer::LoadSettings(ToolboxIni* ini, const char* section)
{
    mKnownTreasures = GuiUtils::IniToMap<decltype(mKnownTreasures)>(ini, section, "known_treasures");
    OnMapChanged();
}

void TreasureRenderer::SaveSettings(ToolboxIni* ini, const char* section) const
{
    GuiUtils::MapToIni(ini, section, "known_treasures", mKnownTreasures);
}

void TreasureRenderer::Update()
{
    if (!mPendingScan) return;
    auto now = std::chrono::system_clock::now();
    if (mPendingScan->expire > now) return;
    constexpr auto radiusSq = kScanRadius * kScanRadius;
    auto [first, last] = std::ranges::remove_if(mExpectedTreasures, [origin = mPendingScan->pos, threshold = now - kScanThreshold](const PendingTreasure& v) {
        return GW::GetSquareDistance(v.pos, origin) < radiusSq && v.lastPing < threshold;
    });
    log("Ping expires, {} expected treasures not found", last - first);
    mExpectedTreasures.erase(first, last);
    mPendingScan = {};
}

void TreasureRenderer::Render(IDirect3DDevice9* device)
{
    if (mExpectedTreasures.empty()) return;

    constexpr int kSegCount = 32, kVertexCount = kSegCount + 1;
    constexpr float kDeltaPhi = DirectX::XM_2PI / kSegCount;

    if (!mCircleVB) {
        // first time init...
        device->CreateVertexBuffer(sizeof(D3DVertex) * kVertexCount, D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX, D3DPOOL_MANAGED, &mCircleVB, nullptr);
        D3DVertex* vertices = nullptr;
        mCircleVB->Lock(0, sizeof(D3DVertex) * kVertexCount, reinterpret_cast<void**>(&vertices), D3DLOCK_DISCARD);
        ASSERT(vertices != nullptr);
        for (int i = 0; i < kVertexCount; ++i) {
            auto phi = i * kDeltaPhi;
            vertices[i] = {kFindRadius * cos(phi), kFindRadius * sin(phi), 0, Colors::DeepPurple()};
        }
        mCircleVB->Unlock();
    }

    D3DMATRIX reset_world;
    device->GetTransform(D3DTS_WORLD, &reset_world);
    device->SetFVF(D3DFVF_CUSTOMVERTEX);
    device->SetStreamSource(0, mCircleVB, 0, sizeof(D3DVertex));
    for (auto& v : mExpectedTreasures) {
        auto world = DirectX::XMMatrixTranslation(v.pos.x, v.pos.y, 0.0f);
        device->SetTransform(D3DTS_WORLD, reinterpret_cast<D3DMATRIX*>(&world));
        device->DrawPrimitive(D3DPT_LINESTRIP, 0, kSegCount);
    }
    device->SetTransform(D3DTS_WORLD, &reset_world);
}

void TreasureRenderer::OnMapChanged()
{
    mExpectedTreasures.clear();
    auto it = mKnownTreasures.find(GW::Map::GetMapID());
    auto haveLD = haveLightOfDeldrimor();
    log("Map change: {} ({}), {} entries, have-ld={}", std::to_underlying(GW::Map::GetMapID()), std::to_underlying(GW::Map::GetInstanceType()), it->second.size() , haveLD);
    if (haveLD && it != mKnownTreasures.end()) {
        mExpectedTreasures.reserve(it->second.size());
        std::ranges::transform(it->second, std::back_inserter(mExpectedTreasures), [](const Treasure& v) -> PendingTreasure { return {v}; });
    }
}

void TreasureRenderer::OnTreasurePing(GW::Vec2f pos, bool found)
{
    auto it = std::ranges::find_if(mExpectedTreasures, [pos](const PendingTreasure& v) {
        return v.pos == pos;
    });
    if (it == mExpectedTreasures.end()) {
        log("New treasure found at [{}, {}]", pos.x, pos.y);
        mKnownTreasures[GW::Map::GetMapID()].push_back({pos});
        mExpectedTreasures.push_back({pos});
        it = mExpectedTreasures.end() - 1;
    }
    it->lastPing = std::chrono::system_clock::now();
    if (found) {
        log("Treasure uncovered at [{}, {}]", pos.x, pos.y);
        mExpectedTreasures.erase(it);
    }
}
