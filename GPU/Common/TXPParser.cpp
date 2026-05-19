#include "GPU/Common/TXPParser.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <zlib.h>
#include <png.h>
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"

#pragma pack(push, 1)
struct TxpHeader {
	u32 flag;
	u32 strcode;
	u32 numImage;
	u32 numInfo;
	u32 numColour;
	u32 imageOffset;
	u32 infoOffset;
	u32 clutOffset;
};

struct TxpImage {
	u16 bitsPerPixelFlag;
	u16 width;
	u16 height;
	u16 pad;
	u32 pad0;
	u32 pixelOffset;
	u32 zOffset;
};

struct TxpInfo {
	u32 flag;
	u32 strcode;
	u32 imageOffset;
	u32 clutOffset;
	float uScale;
	float vScale;
	float uOffset;
	float vOffset;
	s16 width;
	s16 height;
	s16 xOffset;
	s16 yOffset;
};

struct TxpColour {
	u8 r;
	u8 g;
	u8 b;
	u8 a;
};
#pragma pack(pop)

static void WritePNG(const Path& path, const u32* data, int w, int h) {
	FILE* fp = fopen(path.ToString().c_str(), "wb");
	if (!fp) return;

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	png_infop info = png_create_info_struct(png);
	png_init_io(png, fp);
	png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < h; y++) {
		png_write_row(png, (png_bytep)(data + y * w));
	}
	png_write_end(png, nullptr);
	png_destroy_write_struct(&png, &info);
	fclose(fp);
}

static bool DecompressZlib(const u8* src, size_t srcSize, std::vector<u8>& out, int decompressedSize) {
	if (srcSize < 4) return false;
	u32 compressedSize = *(const u32*)src;
	const u8* compressedData = src + 4;

	out.resize(decompressedSize);
	z_stream strm = {};
	strm.next_in = (Bytef*)compressedData;
	strm.avail_in = compressedSize;
	strm.next_out = out.data();
	strm.avail_out = decompressedSize;

	// Jayveer's Noesis plugin uses raw deflate (-15 window bits)
	int ret = inflateInit2(&strm, -15);
	if (ret == Z_OK) {
		ret = inflate(&strm, Z_FINISH);
		inflateEnd(&strm);
		if (ret == Z_STREAM_END && (int)strm.total_out == decompressedSize)
			return true;
	}

	// Fallback: standard zlib wrapper
	std::fill(out.begin(), out.end(), 0);
	strm = {};
	strm.next_in = (Bytef*)compressedData;
	strm.avail_in = compressedSize;
	strm.next_out = out.data();
	strm.avail_out = decompressedSize;
	if (inflateInit(&strm) == Z_OK) {
		inflate(&strm, Z_FINISH);
		inflateEnd(&strm);
		if ((int)strm.total_out == decompressedSize)
			return true;
	}
	return false;
}

static void Unswizzle4(const u8* src, u8* dst, int width, int height) {
	int pos = 0;
	for (int y = 0; y < height; y++) {
		int yc0 = y * 16;
		int yc1 = y / 8 * (width * 4 - 128);
		for (int x = 0; x < width / 2; x++) {
			int xc0 = x / 16 * 16;
			int xc1 = x / 16 * 128;
			int pixelPos = x - xc0 + xc1 + yc0 + yc1;
			dst[pos++] = src[pixelPos] & 0xF;
			dst[pos++] = src[pixelPos] >> 4;
		}
	}
}

static void Unswizzle8(const u8* src, u8* dst, int width, int height) {
	int pos = 0;
	for (int y = 0; y < height; y++) {
		int yc0 = y * 16;
		int yc1 = y / 8 * (width * 8 - 128);
		for (int x = 0; x < width; x++) {
			int xc0 = x / 16 * 16;
			int xc1 = x / 16 * 128;
			dst[pos++] = src[x - xc0 + xc1 + yc0 + yc1];
		}
	}
}

bool TXP::ParseFile(const Path& filename, std::vector<TXPTexture>& outTextures) {
	std::ifstream fs(filename.ToString(), std::ios::binary);
	if (!fs) return false;

	fs.seekg(0, std::ios::end);
	size_t fileSize = fs.tellg();
	fs.seekg(0, std::ios::beg);
	std::vector<u8> data(fileSize);
	fs.read((char*)data.data(), fileSize);
	if (!fs) return false;

	if (fileSize < sizeof(TxpHeader)) return false;

	TxpHeader* header = (TxpHeader*)data.data();
	if (header->numInfo == 0 || header->infoOffset >= fileSize) return false;

	TxpInfo* infos = (TxpInfo*)&data[header->infoOffset];

	for (u32 i = 0; i < header->numInfo; i++) {
		TxpInfo& info = infos[i];
		if (info.imageOffset >= fileSize || info.clutOffset >= fileSize) continue;

		TxpImage* image = (TxpImage*)&data[info.imageOffset];
		if (!image->pixelOffset) continue;

		int bpp = image->bitsPerPixelFlag & 0x0F;
		int imgW = image->width & 0xFFF;
		int imgH = image->height & 0xFFF;

		// bpp == 6 is raw/DXT-like. Skip for now; add DXT decoder here if you need it.
		if (bpp == 6) continue;

		bool isCompressed = (image->bitsPerPixelFlag & 0xF0) != 0;
		int pixelCount = imgW * imgH;

		const u8* rawPixels = isCompressed ? &data[image->zOffset] : &data[image->pixelOffset];
		std::vector<u8> decompressed;
		if (isCompressed) {
			size_t avail = fileSize - (isCompressed ? image->zOffset : image->pixelOffset);
			if (!DecompressZlib(rawPixels, avail, decompressed, pixelCount)) continue;
			rawPixels = decompressed.data();
		}

		std::vector<u8> indices(pixelCount);
		switch (bpp) {
		case 4:  Unswizzle4(rawPixels, indices.data(), imgW, imgH); break;
		case 5:  Unswizzle8(rawPixels, indices.data(), imgW, imgH); break;
		default: continue; // unknown
		}

		TxpColour* clut = (TxpColour*)&data[info.clutOffset];

		TXPTexture tex;
		tex.strcode = info.strcode;
		tex.width = info.width;
		tex.height = info.height;
		tex.rgba.resize(tex.width * tex.height);

		int idx = 0;
		for (int y = info.yOffset; y < info.yOffset + info.height; y++) {
			for (int x = info.xOffset; x < info.xOffset + info.width; x++) {
				int pixelPos = x + y * imgW;
				u8 index = indices[pixelPos];
				TxpColour& c = clut[index];
				// Original Noesis plugin outputs BGRA. We want RGBA for PNG.
				int pos = idx * 4;
				u32 rgba = (c.r) | (c.g << 8) | (c.b << 16) | (c.a << 24);
				tex.rgba[idx] = rgba;
				idx++;
			}
		}

		outTextures.push_back(std::move(tex));
	}

	return !outTextures.empty();
}

void TXP::BatchExportToPNG(const Path& txpDir, const Path& outDir) {
	File::CreateFullPath(outDir);

	// If you prefer std::filesystem (C++17), swap this loop.
	// PPSSPP already has File::GetFilesInDir in Common/File/FileUtil.h
	std::vector<File::FileInfo> files;
	File::GetFilesInDir(txpDir.ToString(), &files);

	for (auto& file : files) {
		if (file.isDirectory) continue;
		std::string name = file.name;
		std::transform(name.begin(), name.end(), name.begin(), ::tolower);
		if (name.size() < 4 || name.substr(name.size() - 4) != ".txp") continue;

		std::vector<TXPTexture> textures;
		if (!ParseFile(file.fullName, textures)) continue;

		// Strip .txp extension for the PNG prefix
		std::string stem = file.name.substr(0, file.name.size() - 4);

		for (auto& tex : textures) {
			std::string pngName = StringFromFormat("%s_%08X.png", stem.c_str(), tex.strcode);
			Path outPath = outDir / pngName;
			WritePNG(outPath, tex.rgba.data(), tex.width, tex.height);
		}
	}
}
