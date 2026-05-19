#pragma once
#include <vector>
#include <string>
#include "Common/CommonTypes.h"
#include "Common/File/Path.h"

struct TXPTexture {
	u32 strcode;
	int width;
	int height;
	std::vector<u32> rgba; // RGBA8888, row-major
};

namespace TXP {
	// Parse one TXP file from disk. Returns true if at least one texture was extracted.
	bool ParseFile(const Path& filename, std::vector<TXPTexture>& outTextures);

	// Scan a folder for *.txp and dump every internal texture as a PNG into outDir.
	// PNG names:  <txp_stem>_<strcode>.png
	void BatchExportToPNG(const Path& txpDir, const Path& outDir);
}
