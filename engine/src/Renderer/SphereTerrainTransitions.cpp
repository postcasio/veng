#include "SphereTerrainTransitions.h"

#include <algorithm>
#include <unordered_set>

namespace Veng::Renderer
{
    namespace
    {
        // Whether the pool holds a tile resident, and its layer.
        const SphereTerrainTileRecord* FindResident(const SphereTerrainTileOracle& tiles,
                                                    const CubeSphereTileKey key)
        {
            const SphereTerrainTileRecord* record = tiles.Find(key);
            return record != nullptr && record->State == SphereTerrainTileState::Resident ? record
                                                                                          : nullptr;
        }
    }

    SphereTerrainTransitionFrame
    AdvanceSphereTerrainTransitions(SphereTerrainTransitionState& state,
                                    const std::span<const SphereTerrainDrawnTile> selected,
                                    const f32 delta, const SphereTerrainTileOracle& tiles)
    {
        state.Clock += delta;
        const f64 now = state.Clock;
        const f64 arrival = SphereTerrainArrivalSeconds;
        const f64 departure = SphereTerrainDepartureSeconds;

        // Last frame's floors as of now, so a tile changing direction starts from where it stands.
        const auto ArrivalFloor = [&](const u64 key)
        {
            const auto it = state.Arrivals.find(key);
            return it == state.Arrivals.end()
                       ? 0.0
                       : std::clamp(1.0 - ((now - it->second) / arrival), 0.0, 1.0);
        };
        const auto DepartureFloor = [&](const u64 key)
        {
            const auto it = state.Departures.find(key);
            return it == state.Departures.end()
                       ? 0.0
                       : std::clamp((now - it->second) / departure, 0.0, 1.0);
        };

        std::unordered_set<u64> chosen;
        chosen.reserve(selected.size());
        for (const SphereTerrainDrawnTile& tile : selected)
        {
            chosen.insert(tile.Key.Value);
        }

        // Each tile drawn last frame and dropped now, under the selected tile that replaces it.
        std::unordered_map<u64, vector<u64>> groups;
        for (const auto& [key, tile] : state.Drawn)
        {
            if (chosen.contains(key))
            {
                continue;
            }
            for (CubeSphereTileKey up = tile.Key; up.GetLevel() > 0;)
            {
                up = up.GetParent();
                if (chosen.contains(up.Value))
                {
                    groups[up.Value].push_back(key);
                    break;
                }
            }
        }

        SphereTerrainTransitionFrame frame;
        frame.Drawn.reserve(selected.size());
        std::unordered_map<u64, f64> departures;
        std::unordered_set<u64> withheld;
        vector<SphereTerrainDrawnTile> departing;
        for (const auto& [replacing, members] : groups)
        {
            bool running = false;
            bool resident = true;
            for (const u64 key : members)
            {
                const CubeSphereTileKey tile = CubeSphereTileKey::FromValue(key);
                const auto it = state.Departures.find(key);
                const f64 start = it != state.Departures.end()
                                      ? it->second
                                      : now - (ArrivalFloor(key) * departure);
                // Running until the frame after its floor reaches 1, so the tile is drawn wholly
                // in the coarser tile's shape once before that tile replaces it.
                running = running || (now - delta) - start < departure;
                resident = resident && FindResident(tiles, tile) != nullptr &&
                           FindResident(tiles, tile.GetParent()) != nullptr;
                departures[key] = start;
            }
            if (!running || !resident)
            {
                for (const u64 key : members)
                {
                    departures.erase(key);
                }
                continue;
            }
            withheld.insert(replacing);
            for (const u64 key : members)
            {
                SphereTerrainDrawnTile tile = state.Drawn.at(key);
                tile.Layer = FindResident(tiles, tile.Key)->Layer;
                tile.ParentLayer = FindResident(tiles, tile.Key.GetParent())->Layer;
                frame.Floors[key] =
                    static_cast<f32>(std::clamp((now - departures.at(key)) / departure, 0.0, 1.0));
                frame.Departing.push_back(tile.Key);
                departing.push_back(tile);
            }
        }

        std::unordered_map<u64, f64> arrivals;
        for (const SphereTerrainDrawnTile& tile : selected)
        {
            const u64 key = tile.Key.Value;
            if (withheld.contains(key))
            {
                continue;
            }
            frame.Drawn.push_back(tile);

            optional<f64> start;
            if (const auto it = state.Arrivals.find(key); it != state.Arrivals.end())
            {
                start = it->second;
            }
            else if (state.Departures.contains(key))
            {
                // A departing tile selected again morphs back from the floor it had reached.
                start = now - ((1.0 - DepartureFloor(key)) * arrival);
            }
            else if (!state.Drawn.empty() && !state.Drawn.contains(key))
            {
                for (CubeSphereTileKey up = tile.Key; up.GetLevel() > 0;)
                {
                    up = up.GetParent();
                    if (state.Drawn.contains(up.Value))
                    {
                        start = now;
                        break;
                    }
                }
            }
            if (!start.has_value())
            {
                continue;
            }
            const f64 floor = 1.0 - ((now - *start) / arrival);
            if (floor <= 0.0)
            {
                continue;
            }
            arrivals[key] = *start;
            frame.Floors[key] = static_cast<f32>(std::min(floor, 1.0));
            frame.ArrivingTiles++;
        }
        frame.Drawn.insert(frame.Drawn.end(), departing.begin(), departing.end());

        state.Arrivals = std::move(arrivals);
        state.Departures = std::move(departures);
        state.Drawn.clear();
        for (const SphereTerrainDrawnTile& tile : frame.Drawn)
        {
            state.Drawn.emplace(tile.Key.Value, tile);
        }
        return frame;
    }
}
