"""Guard filename-loaded Weather controls against inherited bottom-left layout."""
from pathlib import Path
import xml.etree.ElementTree as ET


def verify(xui):
    panel = ET.parse(xui / "panel_weather_controls.xml").getroot()
    # LLPanel::initPanelXML creates external children before initializing parent layout.
    # LLView::applyXUILayout inherits that empty layout unless the child specifies it.
    positioned = [e for e in panel.iter() if e is not panel and any(
        key in e.attrib for key in ("top", "top_pad", "top_delta", "left", "left_pad"))]
    bad = [e.get("name") for e in positioned if e.get("layout") != "topleft"]
    assert not bad, f"External children inherit uninitialized layout: {bad}"
    ladder = panel.find("radio_group")
    # Without explicit height applyXUILayout falls back to 10px; radio icons are 13px.
    rows = ladder.findall("radio_item")
    assert all(int(row.get("height", "0")) >= 16 for row in rows)
    assert sum(int(row.get("height")) + int(row.get("top_pad")) for row in rows) <= int(ladder.get("height"))
    land = ET.parse(xui / "floater_about_land.xml").getroot()
    water = land.find(".//panel[@name='land_water_panel']")
    assert water is not None, "About Land Water tab missing"
    assert water.find(".//button[@name='land_open_wave_painter']") is not None
    print(f"PASS: {len(positioned)} explicit Weather layouts, radio spacing and Water entry")


if __name__ == "__main__":
    verify(Path(__file__).resolve().parents[1] / "skins/default/xui/en")
