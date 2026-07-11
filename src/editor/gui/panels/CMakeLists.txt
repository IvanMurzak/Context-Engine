# src/editor/gui/panels/ — the M5 built-in editor panels, each a headless library built ON the F0b
# context_gui_uitree tree + the R-EDIT-001 contract, CI-assertable WITHOUT booting CEF (R-EDIT-001
# testable-by-construction editor). One add_subdirectory per panel; the M5 fan-out lands them
# independently (F2 scene-tree, then inspector / Problems / …), each on disjoint dirs. Add new panels
# on their OWN line so concurrent sibling tasks inserting here merge cleanly.

add_subdirectory(scenetree)
