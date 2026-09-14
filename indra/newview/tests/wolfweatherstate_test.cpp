#include "../wolfweatherstate.h"

#include <cassert>
#include <string>

int main()
{
    using Source = WolfWeatherSource;

    assert(wolfWeatherSource(false, false, false, false) == Source::MENU);
    assert(wolfWeatherSource(false, false, false, true) == Source::REGION);
    assert(wolfWeatherSource(false, false, true, true) == Source::PARCEL);
    assert(wolfWeatherSource(false, true, true, true) == Source::PRIM);
    assert(wolfWeatherSource(true, true, true, true) == Source::PREVIEW);
    assert(wolfWeatherUsesRegionArtDirection(Source::MENU));
    assert(wolfWeatherUsesRegionArtDirection(Source::REGION));
    assert(!wolfWeatherUsesRegionArtDirection(Source::PARCEL));
    assert(!wolfWeatherUsesRegionArtDirection(Source::PRIM));
    assert(!wolfWeatherUsesRegionArtDirection(Source::PREVIEW));

    WolfWeatherPreviewRegistry<std::string> previews;
    previews.set(10, "region");
    previews.set(20, "parcel");
    assert(previews.active() && *previews.active() == "parcel");
    previews.clear(10);
    assert(previews.active() && *previews.active() == "parcel");
    previews.clear(20);
    assert(!previews.active());

    previews.set(10, "region-older");
    previews.set(20, "parcel");
    previews.set(10, "region-newer");
    previews.clear(10);
    assert(previews.active() && *previews.active() == "parcel");

    WolfWeatherAsyncGate gate;
    const auto old_target = gate.current();
    assert(gate.accepts(old_target));
    gate.advance();
    assert(!gate.accepts(old_target));
    assert(gate.accepts(gate.current()));

    // A UUID can repeat after A -> B -> A, but the first A visit must stay invalid.
    WolfWeatherAsyncGate region_visit;
    const auto first_a = region_visit.current();
    region_visit.advance();
    const auto b = region_visit.current();
    region_visit.advance();
    const auto second_a = region_visit.current();
    assert(!region_visit.accepts(first_a));
    assert(!region_visit.accepts(b));
    assert(region_visit.accepts(second_a));

    // Inventory preset completion belongs to one visible editor session and exact edit
    // revision. A control edit, Revert/hide, or a new target invalidates the callback.
    WolfWeatherEditGate edits;
    const auto initial_load = edits.capture();
    assert(edits.accepts(initial_load));
    edits.changed();
    assert(!edits.accepts(initial_load));
    const auto after_control = edits.capture();
    assert(edits.accepts(after_control));
    edits.newSession();
    assert(!edits.accepts(after_control));
    assert(edits.accepts(edits.capture()));

    // Save results are durable and scope-local. Unrelated fetch state or the other scope's
    // completion cannot turn a failed region POST into a successful one.
    WolfWeatherSaveResult region_save;
    WolfWeatherSaveResult parcel_save;
    region_save.record(7, false, "conflict");
    parcel_save.record(3, true, std::string());
    assert(region_save.completed(7));
    assert(!region_save.succeeded());
    assert(region_save.error() == "conflict");
    assert(parcel_save.completed(3));
    assert(parcel_save.succeeded());
    assert(!region_save.completed(3));

    // The current accepted region policy always gates parcel weather, regardless of parcel and
    // region response order.
    assert(wolfWeatherParcelApplies(true, true, true, true));
    assert(!wolfWeatherParcelApplies(false, true, true, true));
    assert(!wolfWeatherParcelApplies(true, true, false, true));
    assert(!wolfWeatherParcelApplies(true, true, true, false));

    // Two Load clicks may overlap; only the most recently requested asset is accepted.
    WolfWeatherAsyncGate asset_loads;
    const auto first_load = asset_loads.current();
    asset_loads.advance();
    const auto second_load = asset_loads.current();
    assert(!asset_loads.accepts(first_load));
    assert(asset_loads.accepts(second_load));
}
