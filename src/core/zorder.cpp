#include "core/zorder.hpp"

namespace starfish::core {

PlacementVerdict evaluatePlacement(int ourIndex, int defViewIndex)
{
    if (ourIndex < 0 || defViewIndex < 0)
        return PlacementVerdict::Unknown;
    // Top-first ordering: a greater index means a lower position in z-order.
    return ourIndex > defViewIndex ? PlacementVerdict::BelowIconLayer
                                   : PlacementVerdict::AboveIconLayer;
}

const char* placementVerdictText(PlacementVerdict verdict)
{
    switch (verdict) {
    case PlacementVerdict::BelowIconLayer:
        return "below icon layer";
    case PlacementVerdict::AboveIconLayer:
        return "above icon layer";
    case PlacementVerdict::Unknown:
        break;
    }
    return "unknown (window or icon layer not found)";
}

} // namespace starfish::core
