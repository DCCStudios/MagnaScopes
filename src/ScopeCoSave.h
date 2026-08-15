#pragma once

namespace MagnaScope
{
	// Per-character session state in the F4SE co-save: which magnification
	// variant, secondary sight and reticle each scope is currently on.
	//
	// This is in the co-save rather than a JSON beside the profile because it
	// has to fork when a save forks. In a sidecar file, loading a save from
	// before an optic swap would restore the wrong sight, and two characters
	// carrying the same weapon would share one selection.
	//
	// The record key is (plugin filename, local FormID, omod key), which is
	// already load-order independent -- so unlike a raw-FormID scheme this
	// needs no ResolveFormID fixups and survives load-order changes intact.
	bool RegisterCoSave();
}
