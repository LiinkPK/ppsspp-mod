// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <string>

#include <algorithm>
#include "android/jni/app-android.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Common/System/OSD.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/File/FileUtil.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Common/Render/Text/draw_text.h"
#include "Common/StringUtils.h"
#include "Core/MIPS/MIPSTracer.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/Config.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceGe.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/Core.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/WebServer.h"
#include "Core/Util/PathUtil.h"
#include <cstdarg>
#include <functional>
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/TextureReplacer.h"
#include "GPU/Common/PostShader.h"
#include "GPU/ge_constants.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "UI/GPUDriverTestScreen.h"
#include "UI/DeveloperToolsScreen.h"
#include "UI/DevScreens.h"
#include "UI/DriverManagerScreen.h"
#include "UI/DisplayLayoutScreen.h"
#include "UI/GameSettingsScreen.h"
#include "UI/OnScreenDisplay.h"
#include "UI/IconCache.h"
#include "UI/MiscViews.h"
#include "windows.h"
#include <zlib.h>


#if PPSSPP_PLATFORM(ANDROID)

static bool CheckKgslPresent() {
	constexpr auto KgslPath{ "/dev/kgsl-3d0" };

	return access(KgslPath, F_OK) == 0;
}

static bool SupportsCustomDriver() {
	return android_get_device_api_level() >= 28 && CheckKgslPresent();
}

#else

static bool SupportsCustomDriver() {
#ifdef _DEBUG
	return false;  // change to true to debug driver installation on other platforms
#else
	return false;
#endif
}

#endif

static std::string PostShaderTranslateName(std::string_view value) {
	const ShaderInfo* info = GetPostShaderInfo(value);
	if (info) {
		auto ps = GetI18NCategory(I18NCat::POSTSHADERS);
		return std::string(ps->T(value, info->name));
	}
	else {
		return std::string(value);
	}
}

DeveloperToolsScreen::DeveloperToolsScreen(const Path& gamePath)
	: UITabbedBaseDialogScreen(gamePath, &g_Config.iDeveloperSettingsCurrentTab, TabDialogFlags::AddAutoTitles) {
}

static bool SafeMemcpy(u8* dst, const u8* src, size_t len) {
	__try {
		memcpy(dst, src, len);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void DeveloperToolsScreen::CreateTextureReplacementTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	auto di = GetI18NCategory(I18NCat::DIALOG);

	list->Add(new ItemHeader(dev->T("Texture Replacement")));
	list->Add(new CheckBox(&g_Config.bSaveNewTextures, dev->T("Save new textures")));

	// Dump FROM FOLDER
	list->Add(new Choice(dev->T("Dump All Textures From Folder")))->OnClick.Add([this](UI::EventParams& e) {
		System_BrowseForFolder(GetRequesterToken(), "Select GMO root folder", Path(""), [](const std::string& folder, int) {
			if (folder.empty()) return;

			std::string gameID = g_paramSFO.GetDiscID();

			// Recursively collect all .gmo and .gim files
			struct FileEntry { std::string path; std::string relDir; };
			std::vector<FileEntry> files;
			std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& dir, const std::string& rel) {
				WIN32_FIND_DATAA fd;
				HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
				if (h == INVALID_HANDLE_VALUE) return;
				do {
					std::string name = fd.cFileName;
					if (name == "." || name == "..") continue;
					std::string full = dir + "\\" + name;
					std::string relFull = rel.empty() ? name : rel + "\\" + name;
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
						walk(full, relFull);
					}
					else {
						std::string lower = name;
						for (auto& c : lower) c = (char)tolower((unsigned char)c);
						if (lower.size() >= 4) {
							std::string ext = lower.substr(lower.size() - 4);
							if (ext == ".gmo" || ext == ".gim")
								files.push_back({ full, rel });
						}
					}
				} while (FindNextFileA(h, &fd));
				FindClose(h);
				};
			walk(folder, "");

			auto log2u = [](u32 x) { u32 r = 0; while ((1u << r) < x) r++; return r; };
			const u32 BATCH = 256;
			std::vector<u32> allocs;
			auto FlushAllocs = [&]() {
				for (u32 a : allocs) userMemory.Free(a);
				allocs.clear();
				};

			int processed = 0, dumped = 0;
			for (size_t fi = 0; fi < files.size(); fi++) {
				const std::string& path = files[fi].path;
				const std::string& relDir = files[fi].relDir;

				FILE* gf = fopen(path.c_str(), "rb");
				if (!gf) continue;
				fseek(gf, 0, SEEK_END);
				long fsize = ftell(gf);
				fseek(gf, 0, SEEK_SET);
				if (fsize < 16 || fsize > 16 * 1024 * 1024) { fclose(gf); continue; }
				std::vector<u8> fileData(fsize);
				fread(fileData.data(), 1, fsize, gf);
				fclose(gf);

				std::string fname = path.substr(path.find_last_of("/\\") + 1);
				std::string fnameNoExt = fname.substr(0, fname.rfind('.'));
				std::string fnameLowerCheck = fname;
				for (auto& ch : fnameLowerCheck) ch = (char)tolower((unsigned char)ch);
				bool isGIM = fnameLowerCheck.size() >= 4 && fnameLowerCheck.substr(fnameLowerCheck.size() - 4) == ".gim";
				std::string outDir = isGIM
					? (GetSysDirectory(DIRECTORY_TEXTURES) / gameID / "new").ToString()
					: (GetSysDirectory(DIRECTORY_TEXTURES) / gameID / "new" / fnameNoExt).ToString();

				// Scan for all GIM magic offsets in the file
				struct GimRef { s64 offset; u32 len; };
				std::vector<GimRef> gimOffsets;
				for (int s = 0; s + 24 <= (int)fsize; ) {
					if (memcmp(fileData.data() + s, "MIG.00.1PSP\0", 12) == 0) {
						u32 rootSize = *(u32*)(fileData.data() + s + 20);
						u32 gimLen = 16 + rootSize;
						if (gimLen < 32) gimLen = 32;
						if (s + gimLen > (u32)fsize) gimLen = (u32)fsize - s;
						gimOffsets.push_back({ s, gimLen });
						s += gimLen;
					}
					else {
						s++;
					}
				}
				for (const GimRef& gr : gimOffsets) {
					try {
						u32 gimLen = gr.len;
						if (gr.offset + (s64)gimLen > (s64)fsize)
							gimLen = (u32)((s64)fsize - gr.offset);
						FlushAllocs();
						s64 gimOffset = gr.offset;
						u32 gimPSP = userMemory.Alloc(gimLen, false, "folder_gim");
						if (gimPSP == (u32)-1) {
							FlushAllocs();
							gimPSP = userMemory.Alloc(gimLen, false, "folder_gim");
							if (gimPSP == (u32)-1) continue;
						}
						Memory::MemcpyUnchecked(gimPSP, fileData.data() + gimOffset, gimLen);
						allocs.push_back(gimPSP);

					struct Block { u16 type; u32 addr; u16 fmt, w, h; u32 pos; u16 swizzle; u16 c16; };
					std::vector<Block> blocks;
					{
						u32 pos = 16;
						while (pos + 16 <= gimLen) {
							u16 btype = *(u16*)(fileData.data() + gimOffset + pos);
							u32 blen = *(u32*)(fileData.data() + gimOffset + pos + 4);
							if (blen < 16 || pos + blen > gimLen) break;
							if (btype == 0x02 || btype == 0x03) { pos += 16; continue; }
							if (btype == 0x04 || btype == 0x05) {
								const u8* c = fileData.data() + gimOffset + pos + 16;
								u16 bfmt = *(u16*)(c + 4);
								u16 bw = *(u16*)(c + 8);
								u16 bh = *(u16*)(c + 10);
								u32 nib = *(u32*)(c + 24);
								u32 foff = (nib + 4 <= blen - 16) ? *(u32*)(c + nib) : 0;
								u32 pixelPSP = gimPSP + pos + 16 + foff;
								u16 bswizzle = *(u16*)(fileData.data() + gimOffset + pos + 16 + 12);
								u16 bc16 = *(u16*)(fileData.data() + gimOffset + pos + 16 + 16);
								blocks.push_back({ btype, pixelPSP, bfmt, bw, bh, pos, bswizzle, bc16 });
							}
							pos += blen;
						}
					}

					for (size_t bi = 0; bi < blocks.size(); bi++) {
						if (blocks[bi].type != 0x04) continue;
						const Block& img = blocks[bi];

						u32 clutAddr = 0; u16 clutFmt = 0; u32 clutEntries = 0;
						size_t imgIdx = 0;
						for (size_t bk = 0; bk < bi; bk++)
							if (blocks[bk].type == 0x04) imgIdx++;
						size_t palIdx = 0;
						for (size_t bj = 0; bj < blocks.size(); bj++) {
							if (blocks[bj].type != 0x05) continue;
							if (palIdx == imgIdx) {
								clutAddr = blocks[bj].addr;
								clutFmt = blocks[bj].fmt;
								clutEntries = blocks[bj].w;
								break;
							}
							palIdx++;
						}

						if (img.w == 0 || img.h == 0) continue;
						File::CreateFullPath(Path(outDir));
						std::string fnameLower = fname;
						for (auto& ch : fnameLower) ch = (char)tolower((unsigned char)ch);
						bool isGMO = fnameLower.size() >= 4 && fnameLower.substr(fnameLower.size() - 4) == ".gmo";
						bool hasGeometry = isGMO && (std::search(fileData.begin(), fileData.end(), (const u8*)"Shape", (const u8*)"Shape" + 5) != fileData.end());
						if (hasGeometry && img.c16 == 8 && img.w > 0 && img.h > 0) {
							int pitch = (img.fmt == GE_TFMT_CLUT4) ? (img.w / 2) : img.w;
							int bxc = pitch / 16;
							int byc = img.h / 8;
							if (bxc > 0 && byc > 0 && (pitch % 16) == 0) {
								u32 unswizSize = (u32)pitch * (u32)img.h;
								u32 unswizPSP = userMemory.Alloc(unswizSize, false, "unswiz");
								if (unswizPSP != (u32)-1) {
									if (!Memory::IsValidAddress(img.addr) || !Memory::IsValidAddress(unswizPSP)) { userMemory.Free(unswizPSP); continue; }
									DoUnswizzleTex16(Memory::GetPointerUnchecked(img.addr), (u32*)Memory::GetPointerUnchecked(unswizPSP), bxc, byc, pitch);
									allocs.push_back(unswizPSP);
									gpu->GetTextureCacheCommon()->DumpTextureDirect(unswizPSP, img.w, img.h, img.fmt, clutAddr, clutFmt, clutEntries, outDir);
									dumped++;
									continue;
								}
							}
						}
						gpu->GetTextureCacheCommon()->DumpTextureDirect(img.addr, img.w, img.h, img.fmt, clutAddr, clutFmt, clutEntries, outDir);
						dumped++;

					}
					}
					catch (...) {
						FlushAllocs();
						continue;
					}
				}
				if (allocs.size() >= BATCH) FlushAllocs();
			}
			FlushAllocs();
			processed++;

			char msg[128];
			snprintf(msg, sizeof(msg), "Folder done: %d files, %d images", processed, dumped);
			g_OSD.Show(OSDType::MESSAGE_INFO, msg, 5.0f);
		});
	});


	// Dump FROM ISO

	list->Add(new Choice(dev->T("Dump All Textures From ISO")))->OnClick.Add([](UI::EventParams& e) {
		std::string gameID = g_paramSFO.GetDiscID();


		// Recursively collect every .gmo and .gim file from the mounted ISO.
		std::vector<std::string> files;
		std::function<void(const std::string&)> walk = [&](const std::string& dir) {
			bool exists = false;
			auto listing = pspFileSystem.GetDirListing(dir, &exists);
			if (!exists) return;
			for (const auto& info : listing) {
				if (info.name == "." || info.name == "..") continue;
				std::string full = dir + "/" + info.name;
				if (info.type == FILETYPE_DIRECTORY) {
					walk(full);
				}
				else {
					std::string lower = info.name;
					for (auto& c : lower) c = (char)tolower((unsigned char)c);
					if (lower.size() >= 4) {
						std::string ext = lower.substr(lower.size() - 4);
						if (ext == ".gmo" || ext == ".gim") files.push_back(full);
					}
				}
			}
			};
		bool exists = false;
		auto rootListing = pspFileSystem.GetDirListing("disc0:/", &exists);

		// List ALL files regardless of extension to see what's in there
		std::vector<std::string> allFiles;
		std::function<void(const std::string&)> walkAll = [&](const std::string& dir) {
			bool ex = false;
			auto ls = pspFileSystem.GetDirListing(dir, &ex);
			if (!ex) return;
			for (const auto& inf : ls) {
				if (inf.name == "." || inf.name == "..") continue;
				std::string full = dir + "/" + inf.name;
				if (inf.type == FILETYPE_DIRECTORY) {
					walkAll(full);
				}
				else {
					allFiles.push_back(full);
				}
			}
			};
		// First, collect all files on disc
		walkAll("disc0:/PSP_GAME/USRDIR");

		// Temp: log all extensions found
		std::map<std::string, int> extCount;
		for (const auto& af : allFiles) {
			size_t dot = af.rfind('.');
			std::string ext = (dot != std::string::npos) ? af.substr(dot) : "(none)";
			for (auto& c : ext) c = (char)tolower((unsigned char)c);
			extCount[ext]++;
		}
		for (const auto& kv : extCount)

		// Also check for standalone .gim/.gmo files
		for (const auto& af : allFiles) {
			std::string lower = af;
			for (auto& c : lower) c = (char)tolower((unsigned char)c);
			if (lower.size() >= 4) {
				std::string ext = lower.substr(lower.size() - 4);
				if (ext == ".gmo" || ext == ".gim" || ext == ".txp") files.push_back(af);
			}
		}



		// Scan ALL files (including .bin, .dat, .pac, etc.) for embedded GIM magic
		struct GimEntry { std::string srcFile; s64 offset; u32 len; };
		std::vector<GimEntry> gimEntries;

		for (const auto& af : allFiles) {
			int fd = pspFileSystem.OpenFile(af, FILEACCESS_READ);
			if (fd < 0) continue;
			PSPFileInfo pkgInfo = pspFileSystem.GetFileInfoByHandle(fd);
			s64 pkgSize = pkgInfo.size;
			if (pkgSize < 16) { pspFileSystem.CloseFile(fd); continue; }

			// Scan for MIG.00.1PSP magic in 4MB chunks with 64KB overlap
			const size_t CHUNK = 4 * 1024 * 1024;
			const size_t OVERLAP = 64 * 1024;
			std::vector<u8> chunk(CHUNK);
			s64 offset = 0;

			while (offset < pkgSize) {
				size_t toRead = (size_t)((s64)CHUNK < (pkgSize - offset) ? (s64)CHUNK : (pkgSize - offset));
				pspFileSystem.SeekFile(fd, (s32)offset, FILEMOVE_BEGIN);
				pspFileSystem.ReadFile(fd, chunk.data(), toRead);

				for (size_t s = 0; s + 16 <= toRead; s++) {
					if (memcmp(chunk.data() + s, "MIG.00.1PSP\0", 12) == 0) {
						// Estimate GIM length from first block after header
						u32 gimLen = 0;
						if (s + 16 + 8 <= toRead) {
							// Block at +16: type(2) + unk(2) + size(4)
							u32 rootSize = *(u32*)(chunk.data() + s + 16 + 4);
							gimLen = 16 + rootSize;
						}
						if (s + 4 <= toRead && memcmp(chunk.data() + s, "TIM2", 4) == 0) {
							static int tim2Count = 0;
							tim2Count++;
						}
						if (gimLen < 32) gimLen = 32;
						if (gimLen > 2 * 1024 * 1024) gimLen = 2 * 1024 * 1024;
						gimEntries.push_back({ af, offset + (s64)s, gimLen });
					}
				}

				if (toRead == CHUNK)
					offset += CHUNK - OVERLAP;
				else
					break;
			}

			pspFileSystem.CloseFile(fd);
		}

		// ── Collect standalone .txp files ──
		struct TxpEntry { std::string srcFile; };
		std::vector<TxpEntry> txpEntries;
		for (const auto& af : allFiles) {
			std::string lower = af;
			for (auto& c : lower) c = (char)tolower((unsigned char)c);
			if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".txp")
				txpEntries.push_back({ af });
		}

		const u32 BATCH = 256;
		auto CMD = [](u8 cmd, u32 data) -> u32 { return ((u32)cmd << 24) | (data & 0xFFFFFF); };
		std::vector<u32> allocs;
		auto FlushAllocs = [&]() {
			for (u32 a : allocs) userMemory.Free(a);
			allocs.clear();
			};

		int processed = 0, dumped = 0;
		for (size_t fi = 0; fi < gimEntries.size(); fi++) {
			const GimEntry& ge = gimEntries[fi];

			std::vector<u8> gim((size_t)ge.len);
			int gfd = pspFileSystem.OpenFile(ge.srcFile, FILEACCESS_READ);
			if (gfd < 0) continue;
			pspFileSystem.SeekFile(gfd, (s32)ge.offset, FILEMOVE_BEGIN);
			pspFileSystem.ReadFile(gfd, gim.data(), ge.len);
			pspFileSystem.CloseFile(gfd);

			u32 gimLen = ge.len;
			u32 gimSize = gimLen;
			u32 gimPSP = userMemory.Alloc(gimSize, false, "iso_gmo");
			if (gimPSP == (u32)-1) {
				FlushAllocs();
				gimPSP = userMemory.Alloc(gimSize, false, "iso_gmo");
				if (gimPSP == (u32)-1) continue;
			}
			Memory::MemcpyUnchecked(gimPSP, gim.data(), gimLen);
			allocs.push_back(gimPSP);

			struct Block { u16 type; u32 addr; u16 fmt, w, h; u32 pos; u16 swizzle; u16 c16; };
			std::vector<Block> blocks;
			{
				u32 pos = 16;
				while (pos + 16 <= gimLen) {
					u16 btype = *(u16*)(gim.data() + pos);
					u32 blen = *(u32*)(gim.data() + pos + 4);
					if (blen < 16 || pos + blen > gimLen) break;
					if (btype == 0x02 || btype == 0x03) { pos += 16; continue; }
					if (btype == 0x04 || btype == 0x05) {
						const u8* c = gim.data() + pos + 16;
						u16 bfmt = *(u16*)(c + 4);
						u16 bw = *(u16*)(c + 8);
						u16 bh = *(u16*)(c + 10);
						u32 nib = *(u32*)(c + 24);
						u32 foff = (nib + 4 <= blen - 16) ? *(u32*)(c + nib) : 0;
						u32 pixelPSP = gimPSP + pos + 16 + foff;
						u16 bswizzle = *(u16*)(gim.data() + pos + 16 + 12);
						u16 bc16 = *(u16*)(gim.data() + pos + 16 + 16);
						blocks.push_back({ btype, pixelPSP, bfmt, bw, bh, pos, bswizzle, bc16 });
					}
					pos += blen;
				}
			}

			for (size_t bi = 0; bi < blocks.size(); bi++) {
				if (blocks[bi].type != 0x04) continue;
				const Block& img = blocks[bi];

				u32 clutAddr = 0; u16 clutFmt = 0; u32 clutEntries = 0;
				// Count which image block index this is
				size_t imgIdx = 0;
				for (size_t bk = 0; bk < bi; bk++)
					if (blocks[bk].type == 0x04) imgIdx++;
				// Find the palette block at the same index
				size_t palIdx = 0;
				for (size_t bj = 0; bj < blocks.size(); bj++) {
					if (blocks[bj].type != 0x05) continue;
					if (palIdx == imgIdx) {
						clutAddr = blocks[bj].addr;
						clutFmt = blocks[bj].fmt;
						clutEntries = blocks[bj].w;
						break;
					}
					palIdx++;
				}

				if (img.w == 0 || img.h == 0) continue;
				{
					if (img.c16 == 8 && img.w > 0 && img.h > 0) {
						int pitch = (img.fmt == GE_TFMT_CLUT4) ? (img.w / 2) : img.w;
						int bxc = pitch / 16;
						int byc = img.h / 8;
						if (bxc > 0 && byc > 0 && (pitch % 16) == 0) {
							u32 unswizSize = (u32)pitch * (u32)img.h;
							u32 unswizPSP = userMemory.Alloc(unswizSize, false, "unswiz");
							if (unswizPSP != (u32)-1) {
								if (Memory::IsValidAddress(img.addr) && Memory::IsValidAddress(unswizPSP)) {
									DoUnswizzleTex16(Memory::GetPointerUnchecked(img.addr), (u32*)Memory::GetPointerUnchecked(unswizPSP), bxc, byc, pitch);
									allocs.push_back(unswizPSP);
									gpu->GetTextureCacheCommon()->DumpTextureDirect(unswizPSP, img.w, img.h, img.fmt, clutAddr, clutFmt, clutEntries);
									dumped++;
									goto next_block;
								}
							}
						}
					}
					gpu->GetTextureCacheCommon()->DumpTextureDirect(img.addr, img.w, img.h, img.fmt, clutAddr, clutFmt, clutEntries);
					dumped++;
				}
			next_block:;
			}

			processed++;
			if (allocs.size() >= BATCH) FlushAllocs();
		}
		FlushAllocs();

		// ── Peace Walker SLOT.DAT decryption + TXP extraction ──
		{
			// Locate SLOT.DAT and SLOT.KEY on ISO
			std::string slotDatPath = "";
			std::string slotKeyPath = "";
			for (const auto& af : allFiles) {
				std::string lower = af;
				for (auto& c : lower) c = (char)tolower((unsigned char)c);
				if (lower.size() >= 8 && lower.substr(lower.size() - 8) == "slot.dat") slotDatPath = af;
				if (lower.size() >= 8 && lower.substr(lower.size() - 8) == "slot.key") slotKeyPath = af;
			}

			if (slotDatPath.empty() || slotKeyPath.empty()) {
				goto slot_done;
			}

			// Read SLOT.KEY
			{
				int kfd = pspFileSystem.OpenFile(slotKeyPath, FILEACCESS_READ);
				PSPFileInfo kinfo = pspFileSystem.GetFileInfo(slotKeyPath);
				std::vector<u8> keyData((size_t)kinfo.size);
				pspFileSystem.ReadFile(kfd, keyData.data(), keyData.size());
				pspFileSystem.CloseFile(kfd);

				// SlotKeyHeader: saltA(4) saltB(4) saltC(4)
				uint32_t saltA = *(uint32_t*)(keyData.data() + 0);
				uint32_t saltB = *(uint32_t*)(keyData.data() + 4);
				uint32_t saltC = *(uint32_t*)(keyData.data() + 8);

				// Count pages: remaining bytes / 12 bytes per SlotKeyEntry
				uint32_t numPages = (uint32_t)((keyData.size() - 12) / 12);


				// Read SLOT.DAT header to get sector size
				int dfd = pspFileSystem.OpenFile(slotDatPath, FILEACCESS_READ);

				// SlotHeader: timestamp(4) version(2) pageSize(2) numPages(2) unknownA(2) unknownB(4)
				u8 slotHdr[16];
				pspFileSystem.ReadFile(dfd, slotHdr, 16);
				uint16_t pageSize = *(uint16_t*)(slotHdr + 6); // multiplied by sector (0x800)
				uint16_t numPagesH = *(uint16_t*)(slotHdr + 8);
				pspFileSystem.CloseFile(dfd);

				const uint32_t SECTOR = 0x800;

				// Decryptor constants
				const uint32_t DECRYPT_KEY = 0x02E90EDD;

				auto makePageKeyA = [](uint32_t salt) -> uint32_t {
					uint32_t k = salt ^ 0x00006576;
					k <<= 16;
					k |= salt;
					return k;
					};

				auto decodeBuffer = [&](uint32_t keyA, uint32_t keyB, uint32_t size, uint8_t* src) -> uint32_t {
					uint32_t* p = (uint32_t*)src;
					size /= 4;
					for (uint32_t i = 0; i < size; i++) {
						uint32_t interval = keyA * DECRYPT_KEY;
						p[i] ^= keyA;
						keyA = interval + keyB;
					}
					return keyA;
					};

				// Process each page
				for (uint32_t pageID = 0; pageID < numPages && pageID < numPagesH; pageID++) {
					// Read SlotKeyEntry for this page: firstPage(4) lastPage(4) hash(4)
					uint32_t entryOff = 12 + pageID * 12;
					if (entryOff + 12 > keyData.size()) break;
					uint32_t firstPage = *(uint32_t*)(keyData.data() + entryOff + 0);
					uint32_t lastPage = *(uint32_t*)(keyData.data() + entryOff + 4);

					uint32_t start = (firstPage & 0xFFFFF) * SECTOR;
					uint32_t end = (lastPage & 0xFFFFF) * SECTOR;
					uint32_t size = end - start;
					if (size == 0 || size > 8 * 1024 * 1024) continue;

					// Read encrypted page from SLOT.DAT via ISO filesystem
					int pfd = pspFileSystem.OpenFile(slotDatPath, FILEACCESS_READ);
					if (pfd < 0) continue;
					pspFileSystem.SeekFile(pfd, (s32)start, FILEMOVE_BEGIN);
					std::vector<u8> encPage(size);
					pspFileSystem.ReadFile(pfd, encPage.data(), size);
					pspFileSystem.CloseFile(pfd);

					// Decrypt: genericDecode(0, saltA, saltB, saltC, 0, size, data, makeKey=true)
					uint32_t pageKey = saltA ^ saltB;
					uint32_t keyA = makePageKeyA(pageKey);
					uint32_t keyB = pageKey * saltC;
					decodeBuffer(keyA, keyB, size, encPage.data());

					// Decompress: SlotCompressedHeader: u16 u16 u32 compressedSize(4) decompressedSize(4) data[]
					if (size < 16) continue;
					uint32_t compSize = *(uint32_t*)(encPage.data() + 8);
					uint32_t decompSize = *(uint32_t*)(encPage.data() + 12);
					if (decompSize == 0 || decompSize > 16 * 1024 * 1024) continue;

					std::vector<u8> page(decompSize);
					uLongf destLen = decompSize;
					int zr = uncompress(page.data(), &destLen, encPage.data() + 16, compSize);
					if (zr != Z_OK) {
						continue;
					}

					// Parse CNF binary: DataCNF = numTags(4) + tags[]{id(4),offset(4)}
					if (destLen < 8) continue;
					uint32_t numTags = *(uint32_t*)(page.data());
					uint32_t cnfSize = 4 + numTags * 8;
					if (cnfSize >= destLen) continue;

					// dataPtr starts right after the CNF tag table
					// First 0x7F flag with flagID==0 does NOT align — only non-zero flags do
					uint64_t dataPtr = cnfSize;

					for (uint32_t ti = 0; ti + 1 < numTags; ti++) {
						uint32_t tagId = *(uint32_t*)(page.data() + 4 + ti * 8);
						uint32_t tagOff = *(uint32_t*)(page.data() + 4 + ti * 8 + 4);
						uint32_t nextOff = *(uint32_t*)(page.data() + 4 + (ti + 1) * 8 + 4);
						uint8_t  typeId = (uint8_t)(tagId >> 24);

						if (typeId == 0x7F) {
							uint32_t flagID = tagId & 0xFFFFFF;
							if (flagID != 0) {
								// align dataPtr to next sector boundary
								dataPtr = ((dataPtr + SECTOR - 1) / SECTOR) * SECTOR;
							}
							continue;
						}
						if (typeId == 0x7E || typeId == 0x7D || typeId == 0x00) continue;
						if (typeId != 0x14) continue; // only TXP

						uint32_t fileSize = (nextOff > tagOff) ? nextOff - tagOff : 0;
						if (fileSize < 32) continue;
						uint64_t fileStart = dataPtr + tagOff;
						if (fileStart + fileSize > destLen) continue;

						u8* txpData = page.data() + fileStart;

						// Parse TXP header
						uint32_t numColour = *(uint32_t*)(txpData + 16);
						uint32_t infoOffset = *(uint32_t*)(txpData + 24);
						uint32_t clutOffset = *(uint32_t*)(txpData + 28);
						if (numColour == 0 || numColour > 256) continue;
						if (clutOffset + numColour * 4 > fileSize) continue;
						if (infoOffset + 0x24 > fileSize) continue;

						uint32_t dims = *(uint32_t*)(txpData + infoOffset + 0x20);
						int width = (int)(dims & 0xFFFF);
						int height = (int)(dims >> 16);
						if (width == 0 || height == 0 || width > 1024 || height > 1024) continue;

						bool is4bpp = (numColour <= 16);

						// Pixel offset from TXPImage.f3 or .f4
						uint32_t imgBase = *(uint32_t*)(txpData + 20);
						uint32_t pixOff_f3 = *(uint32_t*)(txpData + imgBase + 12);
						uint32_t pixOff_f4 = *(uint32_t*)(txpData + imgBase + 16);
						uint32_t pixOff = (pixOff_f3 > 0) ? pixOff_f3 :
							(pixOff_f4 > 0) ? pixOff_f4 :
							(clutOffset + numColour * 4);
						if (pixOff >= fileSize) continue;

						// Load entire TXP into PSP RAM — same as GIM approach
						u32 txpPSP = userMemory.Alloc((u32)fileSize, false, "txp_data");
						if (txpPSP == (u32)-1) { FlushAllocs(); txpPSP = userMemory.Alloc((u32)fileSize, false, "txp_data"); }
						if (txpPSP == (u32)-1) continue;
						Memory::MemcpyUnchecked(txpPSP, txpData, fileSize);
						allocs.push_back(txpPSP);

						// PSP addresses derived from base — same as GIM's pixelPSP = gimPSP + offset
						u32 pspPix = txpPSP + pixOff;
						u32 pspClut = txpPSP + clutOffset;

						u16 texW = (u16)width;
						u16 texH = (u16)height;
						u16 texFmt = is4bpp ? 3 : 4; // GE_TFMT_CLUT4=3, GE_TFMT_CLUT8=4
						u16 texClutFmt = 3;               // RGBA8888
						u32 texClutEnts = numColour;

						gpu->GetTextureCacheCommon()->DumpTextureDirect(
							pspPix, texW, texH, texFmt,
							pspClut, texClutFmt, texClutEnts);

						dumped++;
						if (allocs.size() >= BATCH) FlushAllocs();
					}

					processed++;
				}
			}
		slot_done:;
		}

		char msg[128];
		snprintf(msg, sizeof(msg), "ISO done: %d files, %d images", processed, dumped);
		g_OSD.Show(OSDType::MESSAGE_INFO, msg, 5.0f);
		});

	list->Add(new CheckBox(&g_Config.bReplaceTextures, dev->T("Replace textures")));

	Choice* createTextureIni = list->Add(new Choice(dev->T("Create/Open textures.ini file for current game")));
	createTextureIni->OnClick.Handle(this, &DeveloperToolsScreen::OnOpenTexturesIniFile);
	createTextureIni->SetEnabledFunc([&] {
		if (!PSP_IsInited())
			return false;

		// Disable the choice to Open/Create if the textures.ini file already exists, and we can't open it due to platform support limitations.
		if (!System_GetPropertyBool(SYSPROP_SUPPORTS_OPEN_FILE_IN_EDITOR)) {
			if (hasTexturesIni_ == HasIni::MAYBE)
				hasTexturesIni_ = TextureReplacer::IniExists(g_paramSFO.GetDiscID()) ? HasIni::YES : HasIni::NO;
			return hasTexturesIni_ != HasIni::YES;
		}
		return true;
		});

	if (System_GetPropertyBool(SYSPROP_CAN_SHOW_FILE)) {
		// Best string we have
		list->Add(new Choice(di->T("Show in folder")))->OnClick.Add([=](UI::EventParams&) {
			Path path;
			if (PSP_IsInited()) {
				std::string gameID = g_paramSFO.GetDiscID();
				path = GetSysDirectory(DIRECTORY_TEXTURES) / gameID;
			}
			else {
				// Just show the root textures directory.
				path = GetSysDirectory(DIRECTORY_TEXTURES);
			}
			System_ShowFileInFolder(path);
			});
	}

	static const char* texLoadSpeeds[] = { "Slow (smooth)", "Medium", "Fast", "Instant (may stutter)" };
	PopupMultiChoice* texLoadSpeed = list->Add(new PopupMultiChoice(&g_Config.iReplacementTextureLoadSpeed, dev->T("Replacement texture load speed"), texLoadSpeeds, 0, ARRAY_SIZE(texLoadSpeeds), I18NCat::DEVELOPER, screenManager()));
	texLoadSpeed->SetChoiceIcon(3, ImageID("I_WARNING"));
}

void DeveloperToolsScreen::CreateGeneralTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	auto gr = GetI18NCategory(I18NCat::GRAPHICS);
	auto ms = GetI18NCategory(I18NCat::MAINSETTINGS);

	list->Add(new ItemHeader(sy->T("CPU Core")));

	bool canUseJit = System_GetPropertyBool(SYSPROP_CAN_JIT);
	// iOS can now use JIT on all modes, apparently.
	// The bool may come in handy for future non-jit platforms though (UWP XB1?)
	// In iOS App Store builds, we disable the JIT.

	static const char* cpuCores[] = { "Interpreter", "Dynarec/JIT (recommended)", "IR Interpreter", "JIT using IR" };
	PopupMultiChoice* core = list->Add(new PopupMultiChoice(&g_Config.iCpuCore, sy->T("CPU Core"), cpuCores, 0, ARRAY_SIZE(cpuCores), I18NCat::SYSTEM, screenManager()));
	core->OnChoice.Add([=](UI::EventParams& e) {
		OnJitAffectingSetting(e);
		g_Config.NotifyUpdatedCpuCore();
		});
	if (!canUseJit) {
		core->HideChoice(1);
		core->HideChoice(3);
	}
	// TODO: Enable "JIT using IR" on more architectures.
#if !PPSSPP_ARCH(X86) && !PPSSPP_ARCH(AMD64) && !PPSSPP_ARCH(ARM64)
	core->HideChoice(3);
#endif

	list->Add(new Choice(dev->T("JIT debug tools")))->OnClick.Handle(this, &DeveloperToolsScreen::OnJitDebugTools);
	list->Add(new CheckBox(&g_Config.bShowDeveloperMenu, dev->T("Show in-game developer menu")));

	AddOverlayList(list, screenManager());

	list->Add(new ItemHeader(sy->T("General")));

	list->Add(new CheckBox(&g_Config.bEnableLogging, dev->T("Enable Logging")))->OnClick.Handle(this, &DeveloperToolsScreen::OnLoggingChanged);
	list->Add(new Choice(dev->T("Logging Channels")))->OnClick.Add([this](UI::EventParams& e) {
		screenManager()->push(new LogConfigScreen());
		});
	list->Add(new CheckBox(&g_Config.bEnableFileLogging, dev->T("Log to file")))->SetEnabledPtr(&g_Config.bEnableLogging);
	if (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) == DEVICE_TYPE_DESKTOP) {
		list->Add(new Choice(dev->T("Show log file in folder")))->OnClick.Add([](UI::EventParams& e) {
			Path logFilePath = g_logManager.GetLogFilePath();
			if (logFilePath.empty()) {
				ERROR_LOG(Log::System, "No log file path configured.");
				return;
			}
			if (File::Exists(logFilePath)) {
				System_ShowFileInFolder(logFilePath);
			}
			else {
				System_LaunchUrl(LaunchUrlType::LOCAL_FILE, logFilePath.NavigateUp().ToString());
			}
			});
	}
	list->Add(new CheckBox(&g_Config.bLogFrameDrops, dev->T("Log Dropped Frame Statistics")));
	if (GetGPUBackend() == GPUBackend::VULKAN) {
		list->Add(new CheckBox(&g_Config.bGpuLogProfiler, dev->T("GPU log profiler")));
	}

	allowDebugger_ = !WebServerStopped(WebServerFlags::DEBUGGER);
	canAllowDebugger_ = !WebServerStopping(WebServerFlags::DEBUGGER);
	CheckBox* allowDebugger = new CheckBox(&allowDebugger_, dev->T("Allow remote debugger"));
	list->Add(allowDebugger)->OnClick.Handle(this, &DeveloperToolsScreen::OnRemoteDebugger);
	allowDebugger->SetEnabledPtr(&canAllowDebugger_);

	CheckBox* localDebugger = list->Add(new CheckBox(&g_Config.bRemoteDebuggerLocal, dev->T("Use locally hosted remote debugger")));
	localDebugger->SetEnabledPtr(&allowDebugger_);

	list->Add(new Choice(dev->T("GPI/GPO switches/LEDs")))->OnClick.Add([=](UI::EventParams& e) {
		screenManager()->push(new GPIGPOScreen(dev->T("GPI/GPO switches/LEDs")));
		});

	list->Add(new CheckBox(&g_Config.bShowSaveLoadIndicator, dev->T("Show indicator when saving/loading")));

#if PPSSPP_PLATFORM(ANDROID)
	static const char* framerateModes[] = { "Default", "Request 60 Hz", "Force 60Hz" };
	PopupMultiChoice* framerateMode = list->Add(new PopupMultiChoice(&g_Config.iDisplayFramerateMode, gr->T("Framerate mode"), framerateModes, 0, ARRAY_SIZE(framerateModes), I18NCat::GRAPHICS, screenManager()));
	framerateMode->SetEnabledFunc([]() { return System_GetPropertyInt(SYSPROP_SYSTEMVERSION) >= 30; });
	framerateMode->OnChoice.Add([](UI::EventParams& e) {
		System_Notify(SystemNotification::FORCE_RECREATE_ACTIVITY);
		});
#endif

#if PPSSPP_PLATFORM(IOS)
	list->Add(new NoticeView(NoticeLevel::WARN, ms->T("Moving the memstick directory is NOT recommended on iOS"), ""))->SetWrapText(true);
	list->Add(new Choice(sy->T("Set Memory Stick folder")))->OnClick.Add(
		[=](UI::EventParams&) {
			SetMemStickDirDarwin(GetRequesterToken());
		});
#endif

	// Makes it easy to get savestates out of an iOS device. The file listing shown in MacOS doesn't allow
	// you to descend into directories.
#if PPSSPP_PLATFORM(IOS)
	list->Add(new Choice(dev->T("Copy savestates to memstick root")))->OnClick.Handle(this, &DeveloperToolsScreen::OnCopyStatesToRoot);
#endif

	auto di = GetI18NCategory(I18NCat::DIALOG);
	// Reuse strings to the max, heh.
	list->Add(new Choice(ApplySafeSubstitutions("%1: ppsspp.ini", di->T("Copy to clipboard"))))->OnClick.Add([=](UI::EventParams&) {
		auto di = GetI18NCategory(I18NCat::DIALOG);
		std::string configStr = g_Config.GetConfigAsString();
		if (!configStr.empty()) {
			System_CopyStringToClipboard(configStr);
			g_OSD.Show(OSDType::MESSAGE_INFO, ApplySafeSubstitutions(di->T("Copied to clipboard: %1"), "ppsspp.ini"), 0.0f, "copyToClip");
		}
		});
}

void DeveloperToolsScreen::CreateTestsTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);

	list->Add(new Choice(dev->T("Touchscreen Test")))->OnClick.Add([this](UI::EventParams& e) {
		screenManager()->push(new TouchTestScreen(gamePath_));
		// Handle touchscreen test event
		});
	// list->Add(new Choice(dev->T("Memstick Test")))->OnClick.Handle(this, &DeveloperToolsScreen::OnMemstickTest);
	Choice* frameDumpTests = list->Add(new Choice(dev->T("Framedump tests")));
	frameDumpTests->OnClick.Add([this](UI::EventParams& e) {
		screenManager()->push(new FrameDumpTestScreen());
		});
	frameDumpTests->SetEnabled(!PSP_IsInited());
	// For now, we only implement GPU driver tests for Vulkan and OpenGL. This is simply
	// because the D3D drivers are generally solid enough to not need this type of investigation.
	if (g_Config.iGPUBackend == (int)GPUBackend::VULKAN || g_Config.iGPUBackend == (int)GPUBackend::OPENGL) {
		list->Add(new Choice(dev->T("GPU Driver Test")))->OnClick.Handle(this, &DeveloperToolsScreen::OnGPUDriverTest);
	}

	// Not useful enough to be made visible.
	/*
	auto memmapTest = list->Add(new Choice(dev->T("Memory map test")));
	memmapTest->OnClick.Add([this](UI::EventParams &e) {
		MemoryMapTest();
	});
	memmapTest->SetEnabled(PSP_IsInited());
	*/
}

void DeveloperToolsScreen::CreateDumpFileTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);

	list->Add(new ItemHeader(dev->T("Dump files")));
	list->Add(new BitCheckBox(&g_Config.iDumpFileTypes, (int)DumpFileType::EBOOT, dev->T("Dump Decrypted Eboot", "Dump Decrypted EBOOT.BIN (If Encrypted) When Booting Game")));
	list->Add(new BitCheckBox(&g_Config.iDumpFileTypes, (int)DumpFileType::PRX, dev->T("PRX")));
	list->Add(new BitCheckBox(&g_Config.iDumpFileTypes, (int)DumpFileType::Atrac3, dev->T("Atrac3/3+")));
	list->Add(new BitCheckBox(&g_Config.iDumpFileTypes, (int)DumpFileType::PBP_ISO, dev->T("ISO from PBP")));
}

void DeveloperToolsScreen::CreateHLETab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);

	list->Add(new CheckBox(&g_Config.bUseOldAtrac, dev->T("Use the old sceAtrac implementation")));

	list->Add(new ItemHeader(dev->T("Disable HLE")));

	for (int i = 0; i < (int)DisableHLEFlags::Count; i++) {
		DisableHLEFlags flag = (DisableHLEFlags)(1 << i);

		// Show a checkbox, unless the setting has graduated to always disabled.
		if (!(flag & AlwaysDisableHLEFlags())) {
			const HLEModuleMeta* meta = GetHLEModuleMetaByFlag(flag);
			if (meta) {
				BitCheckBox* checkBox = list->Add(new BitCheckBox(&g_Config.iDisableHLE, (int)flag, meta->modname));
				checkBox->SetEnabled(!PSP_IsInited());
			}
		}
	}

	list->Add(new ItemHeader(dev->T("Force-enable HLE")));

	for (int i = 0; i < (int)DisableHLEFlags::Count; i++) {
		DisableHLEFlags flag = (DisableHLEFlags)(1 << i);

		// Show a checkbox, only if the setting has graduated to always disabled (and thus it makes sense to force-enable it).
		if (flag & AlwaysDisableHLEFlags()) {
			const HLEModuleMeta* meta = GetHLEModuleMetaByFlag(flag);
			if (meta) {
				BitCheckBox* checkBox = list->Add(new BitCheckBox(&g_Config.iForceEnableHLE, (int)flag, meta->modname));
				checkBox->SetEnabled(!PSP_IsInited());
			}
		}
	}
}

void DeveloperToolsScreen::CreateMIPSTracerTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	list->Add(new ItemHeader(dev->T("MIPSTracer")));

	MIPSTracerEnabled_ = mipsTracer.tracing_enabled;
	CheckBox* MIPSTracerEnabled = new CheckBox(&MIPSTracerEnabled_, dev->T("MIPSTracer enabled"));
	list->Add(MIPSTracerEnabled)->OnClick.Handle(this, &DeveloperToolsScreen::OnMIPSTracerEnabled);
	MIPSTracerEnabled->SetEnabledFunc([]() {
		bool temp = g_Config.iCpuCore == static_cast<int>(CPUCore::IR_INTERPRETER) && PSP_IsInited();
		return temp && Core_IsStepping() && coreState != CORE_POWERDOWN;
		});

	Choice* TraceDumpPath = list->Add(new Choice(dev->T("Select the file path for the trace")));
	TraceDumpPath->OnClick.Handle(this, &DeveloperToolsScreen::OnMIPSTracerPathChanged);
	TraceDumpPath->SetEnabledFunc([]() {
		if (!PSP_IsInited())
			return false;
		return true;
		});

	MIPSTracerPath_ = mipsTracer.get_logging_path();
	MIPSTracerPath = list->Add(new InfoItem(dev->T("Current log file"), MIPSTracerPath_));

	PopupSliderChoice* storage_capacity = list->Add(
		new PopupSliderChoice(
			&mipsTracer.in_storage_capacity, 0x4'0000, 0x40'0000, 0x10'0000, dev->T("Storage capacity"), 0x10000, screenManager()
		)
	);
	storage_capacity->SetFormat("0x%x asm opcodes");
	storage_capacity->OnChange.Add([&](UI::EventParams&) {
		INFO_LOG(Log::JIT, "User changed the tracer's storage capacity to 0x%x", mipsTracer.in_storage_capacity);
		});

	PopupSliderChoice* trace_max_size = list->Add(
		new PopupSliderChoice(
			&mipsTracer.in_max_trace_size, 0x1'0000, 0x40'0000, 0x10'0000, dev->T("Max allowed trace size"), 0x10000, screenManager()
		)
	);
	trace_max_size->SetFormat("%d basic blocks");
	trace_max_size->OnChange.Add([&](UI::EventParams&) {
		INFO_LOG(Log::JIT, "User changed the tracer's max trace size to %d", mipsTracer.in_max_trace_size);
		});

	list->Add(new ItemHeader(dev->T("MIPSTracer actions")));
	Choice* FlushTrace = list->Add(new Choice(dev->T("Flush the trace")));
	FlushTrace->OnClick.Handle(this, &DeveloperToolsScreen::OnMIPSTracerFlushTrace);

	Choice* InvalidateJitCache = list->Add(new Choice(dev->T("Clear the JIT cache")));
	InvalidateJitCache->OnClick.Handle(this, &DeveloperToolsScreen::OnMIPSTracerClearJitCache);

	Choice* ClearMIPSTracer = list->Add(new Choice(dev->T("Clear the MIPSTracer")));
	ClearMIPSTracer->OnClick.Handle(this, &DeveloperToolsScreen::OnMIPSTracerClearTracer);
}

void DeveloperToolsScreen::CreateAudioTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	list->Add(new CheckBox(&g_Config.bForceFfmpegForAudioDec, dev->T("Use FFMPEG for all compressed audio")));
}

void DeveloperToolsScreen::CreateUITab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	UIContext* uiContext = screenManager()->getUIContext();
	list->Add(new Choice(dev->T("Reload UI atlas")))->OnClick.Add([uiContext](UI::EventParams&) {
		uiContext->InvalidateAtlas();
		});

	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto si = GetI18NCategory(I18NCat::SYSINFO);
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	auto ac = GetI18NCategory(I18NCat::ACHIEVEMENTS);

	// TODO: Move most of these strings out of the SysInfo category.

	list->Add(new ItemHeader(si->T("Icon cache")));
	IconCacheStats iconStats = g_iconCache.GetStats();
	list->Add(new InfoItem(si->T("Image data count"), StringFromFormat("%d", iconStats.cachedCount)));
	list->Add(new InfoItem(si->T("Texture count"), StringFromFormat("%d", iconStats.textureCount)));
	list->Add(new InfoItem(si->T("Data size"), NiceSizeFormat(iconStats.dataSize)));
	list->Add(new Choice(di->T("Clear")))->OnClick.Add([&](UI::EventParams&) {
		g_iconCache.ClearData();
		RecreateViews();
		});

	list->Add(new ItemHeader(si->T("Font cache")));
	const TextDrawer* text = screenManager()->getUIContext()->Text();
	if (text) {
		list->Add(new InfoItem(si->T("Texture count"), StringFromFormat("%d", text->GetStringCacheSize())));
		list->Add(new InfoItem(si->T("Data size"), NiceSizeFormat(text->GetCacheDataSize())));
	}

	list->Add(new ItemHeader(si->T("Slider test")));
	list->Add(new Slider(&testSliderValue_, 0, 100, 1, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));

	static const char* positions[] = { "Bottom Left", "Bottom Center", "Bottom Right", "Top Left", "Top Center", "Top Right", "Center Left", "Center Right", "None" };

	list->Add(new ItemHeader(si->T("Notification tests")));
	list->Add(new Choice(si->T("Error")))->OnClick.Add([&](UI::EventParams&) {
		std::string str = "Error " + CodepointToUTF8(0x1F41B) + CodepointToUTF8(0x1F41C) + CodepointToUTF8(0x1F914);
		g_OSD.Show(OSDType::MESSAGE_ERROR, str);
		});
	list->Add(new Choice(si->T("Warning")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.Show(OSDType::MESSAGE_WARNING, "Warning, a pretty long warning heading", "Some\nAdditional\nDetail, some of which is very, very long and wide and will need line wrapping on most screens.");
		});
	list->Add(new Choice(si->T("Info")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.Show(OSDType::MESSAGE_INFO, "Info, info info info info info info info info info info");
		});
	// This one is clickable
	list->Add(new Choice(si->T("Success")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.Show(OSDType::MESSAGE_SUCCESS, "Success", 0.0f, "clickable");
		g_OSD.SetClickCallback("clickable", []() {
			System_LaunchUrl(LaunchUrlType::BROWSER_URL, "https://www.google.com/");
			});
		});
	list->Add(new Choice(sy->T("RetroAchievements")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.Show(OSDType::MESSAGE_WARNING, "RetroAchievements warning", "", "I_RETROACHIEVEMENTS_LOGO");
		});
	list->Add(new ItemHeader(si->T("Progress tests")));
	list->Add(new Choice(si->T("30%")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.SetProgressBar("testprogress", "Test Progress", 1, 100, 30, 0.0f);
		});
	list->Add(new Choice(si->T("100%")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.SetProgressBar("testprogress", "Test Progress", 1, 100, 100, 1.0f);
		});
	list->Add(new Choice(si->T("N/A%")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.SetProgressBar("testprogress", "Test Progress", 0, 0, 0, 0.0f);
		});
	list->Add(new Choice(si->T("Success")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.RemoveProgressBar("testprogress", true, 0.5f);
		});
	list->Add(new Choice(si->T("Failure")))->OnClick.Add([&](UI::EventParams&) {
		g_OSD.RemoveProgressBar("testprogress", false, 0.5f);
		});
	list->Add(new ItemHeader(si->T("Achievement tests")));
	list->Add(new Choice(si->T("Leaderboard tracker: Show")))->OnClick.Add([=](UI::EventParams&) {
		g_OSD.ShowLeaderboardTracker(1, "My leaderboard tracker", true);
		});
	list->Add(new Choice(si->T("Leaderboard tracker: Update")))->OnClick.Add([=](UI::EventParams&) {
		g_OSD.ShowLeaderboardTracker(1, "Updated tracker", true);
		});
	list->Add(new Choice(si->T("Leaderboard tracker: Hide")))->OnClick.Add([=](UI::EventParams&) {
		g_OSD.ShowLeaderboardTracker(1, "", false);
		});

	list->Add(new ItemHeader(ac->T("Notifications")));
	list->Add(new PopupMultiChoice(&g_Config.iNotificationPos, sy->T("Notification screen position"), positions, 0, ARRAY_SIZE(positions), I18NCat::DIALOG, screenManager()));
	list->Add(new PopupMultiChoice(&g_Config.iAchievementsLeaderboardTrackerPos, ac->T("Leaderboard tracker"), positions, 0, ARRAY_SIZE(positions), I18NCat::DIALOG, screenManager()));

#ifdef _DEBUG
	list->Add(new CheckBox(&pretendIngame_, ac->T("Pretend to be in-game (for testing)")));
	// Untranslated string because this is debug mode only, only for PPSSPP developers.
	list->Add(new ItemHeader(ac->T("Assert")));
	list->Add(new Choice("Assert"))->OnClick.Add([=](UI::EventParams&) {
		_dbg_assert_msg_(false, "Test assert message");
		});
#endif
#if PPSSPP_PLATFORM(ANDROID)
	list->Add(new Choice(si->T("Exception")))->OnClick.Add([&](UI::EventParams&) {
		System_Notify(SystemNotification::TEST_JAVA_EXCEPTION);
		});
#endif
}

void DeveloperToolsScreen::CreateNetworkTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	auto ms = GetI18NCategory(I18NCat::MAINSETTINGS);
	auto ri = GetI18NCategory(I18NCat::REMOTEISO);
	list->Add(new ItemHeader(ms->T("Networking")));
	list->Add(new CheckBox(&g_Config.bDontDownloadInfraJson, dev->T("Don't download infra-dns.json")));
	list->Add(new CheckBox(&g_Config.bAdhocServerShowPlayerPorts, dev->T("Show player ports in adhoc server status")));
	// This is shared between RemoteISO and the remote debugger.
	PopupSliderChoice* portChoice = new PopupSliderChoice(&g_Config.iRemoteISOPort, 0, 65535, 0, ri->T("Local Server Port", "Local Server Port"), 100, screenManager());
	list->Add(portChoice);
}

// TODO: Make this generic
extern int DefaultDepthRaster();

void DeveloperToolsScreen::CreateGraphicsTab(UI::LinearLayout* list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	auto gr = GetI18NCategory(I18NCat::GRAPHICS);
	auto ps = GetI18NCategory(I18NCat::POSTSHADERS);
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	auto si = GetI18NCategory(I18NCat::SYSINFO);

	Draw::DrawContext* draw = screenManager()->getDrawContext();

	list->Add(new ItemHeader(sy->T("General")));
	list->Add(new CheckBox(&g_Config.bVendorBugChecksEnabled, dev->T("Enable driver bug workarounds")));
	list->Add(new CheckBox(&g_Config.bShaderCache, dev->T("Enable shader cache")));

	auto displayRefreshRate = list->Add(new PopupSliderChoice(&g_Config.iDisplayRefreshRate, 60, 1000, 60, dev->T("Display refresh rate"), 1, screenManager()));
	displayRefreshRate->SetFormat(si->T("%d Hz"));

	list->Add(new ItemHeader(dev->T("Vulkan")));
	list->Add(new CheckBox(&g_Config.bVulkanDisableImplicitLayers, dev->T("Prevent loading overlays")));

	if (g_Config.iGPUBackend == (int)GPUBackend::VULKAN) {
		list->Add(new CheckBox(&g_Config.bRenderMultiThreading, dev->T("Multi-threaded rendering"), ""))->OnClick.Add([](UI::EventParams& e) {
			// TODO: Not translating yet. Will combine with other translations of settings that need restart.
			g_OSD.Show(OSDType::MESSAGE_WARNING, "Restart required");
			});
	}

	if (GetGPUBackend() == GPUBackend::VULKAN && SupportsCustomDriver()) {
		auto driverChoice = list->Add(new Choice(gr->T("AdrenoTools driver manager")));
		driverChoice->OnClick.Add([=](UI::EventParams& e) {
			screenManager()->push(new DriverManagerScreen(gamePath_));
			});
	}

	static const char* depthRasterModes[] = { "Auto", "Low", "Off", "Always on" };

	PopupMultiChoice* depthRasterMode = list->Add(new PopupMultiChoice(&g_Config.iDepthRasterMode, gr->T("Lens flare occlusion"), depthRasterModes, 0, ARRAY_SIZE(depthRasterModes), I18NCat::GRAPHICS, screenManager()));
	depthRasterMode->SetDisabledPtr(&g_Config.bSoftwareRendering);
	depthRasterMode->SetDefault(DefaultDepthRaster());
	depthRasterMode->SetChoiceIcon(3, ImageID("I_WARNING"));  // It's a performance trap.

	list->Add(new ItemHeader(dev->T("Ubershaders")));
	if (draw->GetShaderLanguageDesc().bitwiseOps && !draw->GetBugs().Has(Draw::Bugs::UNIFORM_INDEXING_BROKEN)) {
		// If the above if fails, the checkbox is redundant since it'll be force disabled anyway.
		list->Add(new CheckBox(&g_Config.bUberShaderVertex, dev->T("Vertex")));
	}
#if !PPSSPP_PLATFORM(UWP)
	if (g_Config.iGPUBackend != (int)GPUBackend::OPENGL || gl_extensions.GLES3) {
#else
		{
#endif
			list->Add(new CheckBox(&g_Config.bUberShaderFragment, dev->T("Fragment")));
		}

		// Experimental, allow some VR features without OpenXR
		if (GetGPUBackend() == GPUBackend::OPENGL) {
			auto vr = GetI18NCategory(I18NCat::VR);
			list->Add(new ItemHeader(vr->T("Virtual reality")));
			list->Add(new CheckBox(&g_Config.bForceVR, vr->T("VR camera")));
		}

		// Experimental, will move to main graphics settings later.
		bool multiViewSupported = draw->GetDeviceCaps().multiViewSupported;

		auto enableStereo = [=]() -> bool {
			return g_Config.bStereoRendering && multiViewSupported;
			};

		if (multiViewSupported) {
			list->Add(new ItemHeader(gr->T("Stereo rendering")));
			list->Add(new CheckBox(&g_Config.bStereoRendering, gr->T("Stereo rendering")));
			std::vector<std::string> stereoShaderNames;

			ChoiceWithValueDisplay* stereoShaderChoice = list->Add(new ChoiceWithValueDisplay(&g_Config.sStereoToMonoShader, gr->T("Stereo display shader"), &PostShaderTranslateName));
			stereoShaderChoice->SetEnabledFunc(enableStereo);
			stereoShaderChoice->OnClick.Add([=](EventParams& e) {
				auto gr = GetI18NCategory(I18NCat::GRAPHICS);
				auto procScreen = new PostProcScreen(gr->T("Stereo display shader"), 0, true);
				if (e.v)
					procScreen->SetPopupOrigin(e.v);
				screenManager()->push(procScreen);
				});
			const ShaderInfo* shaderInfo = GetPostShaderInfo(g_Config.sStereoToMonoShader);
			if (shaderInfo) {
				for (size_t i = 0; i < ARRAY_SIZE(shaderInfo->settings); ++i) {
					auto& setting = shaderInfo->settings[i];
					if (!setting.name.empty()) {
						std::string key = StringFromFormat("%sSettingCurrentValue%d", shaderInfo->section.c_str(), i + 1);
						bool keyExisted = g_Config.mPostShaderSetting.find(key) != g_Config.mPostShaderSetting.end();
						auto& value = g_Config.mPostShaderSetting[key];
						if (!keyExisted)
							value = setting.value;

						PopupSliderChoiceFloat* settingValue = list->Add(new PopupSliderChoiceFloat(&value, setting.minValue, setting.maxValue, setting.value, ps->T(setting.name), setting.step, screenManager()));
						settingValue->SetEnabledFunc([=] {
							return !g_Config.bSkipBufferEffects && enableStereo();
							});
					}
				}
			}
		}
	}

void DeveloperToolsScreen::CreateCrashHistoryTab(UI::LinearLayout * list) {
	using namespace UI;
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	std::vector<std::string> reports = Android_GetNativeCrashHistory(20);
	list->Add(new ItemHeader(dev->T("Crash history")));
	if (reports.empty()) {
		list->Add(new TextView(di->T("None")));
		return;
	}
	for (size_t i = 0; i < reports.size(); i++) {
		std::string name = StringFromFormat("Crash %d", (int)i);
		CollapsibleSection* section = list->Add(new CollapsibleSection(name));
		const std::string report = reports[i];
		if (report.size() > 150) {
			section->Add(new Choice(di->T("Copy to clipboard"), ImageID("I_FILE_COPY")))->OnClick.Add([report](UI::EventParams&) {
				System_CopyStringToClipboard(report);
				});
		}
		section->Add(new TextView(report, FLAG_WRAP_TEXT | FLAG_DYNAMIC_ASCII, true));
	}
}

void DeveloperToolsScreen::CreateTabs() {
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	auto ms = GetI18NCategory(I18NCat::MAINSETTINGS);

	AddTab("General", sy->T("General"), [this](UI::LinearLayout* parent) {
		CreateGeneralTab(parent);
		});
	AddTab("TextureReplacement", dev->T("Texture Replacement"), [this](UI::LinearLayout* parent) {
		CreateTextureReplacementTab(parent);
		});
	AddTab("Graphics", ms->T("Graphics"), [this](UI::LinearLayout* parent) {
		CreateGraphicsTab(parent);
		});
	AddTab("Networking", ms->T("Networking"), [this](UI::LinearLayout* parent) {
		CreateNetworkTab(parent);
		});
	AddTab("Audio", ms->T("Audio"), [this](UI::LinearLayout* parent) {
		CreateAudioTab(parent);
		});
	AddTab("Tests", dev->T("Tests"), [this](UI::LinearLayout* parent) {
		CreateTestsTab(parent);
		});
	AddTab("UI", dev->T("UI"), [this](UI::LinearLayout* parent) {
		CreateUITab(parent);
		});
	AddTab("DumpFiles", dev->T("Dump files"), [this](UI::LinearLayout* parent) {
		CreateDumpFileTab(parent);
		});
	// Need a better title string.
	AddTab("HLE", dev->T("Disable HLE"), [this](UI::LinearLayout* parent) {
		CreateHLETab(parent);
		});
#if !PPSSPP_PLATFORM(ANDROID) && !PPSSPP_PLATFORM(IOS) && !PPSSPP_PLATFORM(SWITCH)
	AddTab("MIPSTracer", dev->T("MIPSTracer"), [this](UI::LinearLayout* parent) {
		CreateMIPSTracerTab(parent);
		});
#endif
	//#if PPSSPP_PLATFORM(ANDROID) 
	if (System_GetPropertyInt(SYSPROP_SYSTEMVERSION) >= 30) {
		AddTab("Crash history", dev->T("Crash history"), [this](UI::LinearLayout* parent) {
			CreateCrashHistoryTab(parent);
			});
	}
	//#endif

		// Reconsider whenever recreating views.
	hasTexturesIni_ = HasIni::MAYBE;
}

void DeveloperToolsScreen::onFinish(DialogResult result) {
	UIScreen::onFinish(result);
	g_Config.Save("DeveloperToolsScreen::onFinish");
	System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
}

void DeveloperToolsScreen::OnLoggingChanged(UI::EventParams & e) {
	System_Notify(SystemNotification::TOGGLE_DEBUG_CONSOLE);
}

void DeveloperToolsScreen::OnOpenTexturesIniFile(UI::EventParams & e) {
	std::string gameID = g_paramSFO.GetDiscID();
	Path generatedFilename;

	if (TextureReplacer::GenerateIni(gameID, generatedFilename)) {
		if (System_GetPropertyBool(SYSPROP_SUPPORTS_OPEN_FILE_IN_EDITOR)) {
			File::OpenFileInEditor(generatedFilename);
		}
		else {
			// Can't do much here, let's send a "toast" so the user sees that something happened.
			auto dev = GetI18NCategory(I18NCat::DEVELOPER);
			System_Toast((GetFriendlyPath(generatedFilename) + ": " + dev->T_cstr("Texture ini file created")).c_str());
		}

		hasTexturesIni_ = HasIni::YES;
	}
}

void DeveloperToolsScreen::OnJitDebugTools(UI::EventParams & e) {
	screenManager()->push(new JitDebugScreen());
}

void DeveloperToolsScreen::OnGPUDriverTest(UI::EventParams & e) {
	screenManager()->push(new GPUDriverTestScreen());
}

void DeveloperToolsScreen::OnJitAffectingSetting(UI::EventParams & e) {
	System_PostUIMessage(UIMessage::REQUEST_CLEAR_JIT);
}

void DeveloperToolsScreen::OnCopyStatesToRoot(UI::EventParams & e) {
	Path savestate_dir = GetSysDirectory(DIRECTORY_SAVESTATE);
	Path root_dir = GetSysDirectory(DIRECTORY_MEMSTICK_ROOT);

	std::vector<File::FileInfo> files;
	GetFilesInDir(savestate_dir, &files, nullptr, 0);

	for (const File::FileInfo& file : files) {
		Path src = file.fullName;
		Path dst = root_dir / file.name;
		INFO_LOG(Log::System, "Copying file '%s' to '%s'", src.c_str(), dst.c_str());
		File::Copy(src, dst);
	}
}

void DeveloperToolsScreen::OnRemoteDebugger(UI::EventParams & e) {
	if (allowDebugger_) {
		StartWebServer(WebServerFlags::DEBUGGER);
	}
	else {
		StopWebServer(WebServerFlags::DEBUGGER);
	}
	// Persist the setting.  Maybe should separate?
	g_Config.bRemoteDebuggerOnStartup = allowDebugger_;
}

void DeveloperToolsScreen::OnMIPSTracerEnabled(UI::EventParams & e) {
	if (MIPSTracerEnabled_) {
		u32 capacity = mipsTracer.in_storage_capacity;
		u32 trace_size = mipsTracer.in_max_trace_size;

		mipsTracer.initialize(capacity, trace_size);
		mipsTracer.start_tracing();
	}
	else {
		mipsTracer.stop_tracing();
	}
}

void DeveloperToolsScreen::OnMIPSTracerPathChanged(UI::EventParams & e) {
	auto dev = GetI18NCategory(I18NCat::DEVELOPER);
	System_BrowseForFileSave(
		GetRequesterToken(),
		dev->T("Select the log file"),
		"trace.txt",
		BrowseFileType::ANY,
		[this](const std::string& value, int) {
			mipsTracer.set_logging_path(value);
			MIPSTracerPath_ = value;
			MIPSTracerPath->SetRightText(MIPSTracerPath_);
		}
	);
}

void DeveloperToolsScreen::OnMIPSTracerFlushTrace(UI::EventParams & e) {
	mipsTracer.flush_to_file();
	// The error logs are emitted inside the tracer
}

void DeveloperToolsScreen::OnMIPSTracerClearJitCache(UI::EventParams & e) {
	INFO_LOG(Log::JIT, "Clearing the jit cache...");
	System_PostUIMessage(UIMessage::REQUEST_CLEAR_JIT);
}

void DeveloperToolsScreen::OnMIPSTracerClearTracer(UI::EventParams & e) {
	INFO_LOG(Log::JIT, "Clearing the MIPSTracer...");
	mipsTracer.clear();
}

void DeveloperToolsScreen::update() {
	UIBaseDialogScreen::update();
	allowDebugger_ = !WebServerStopped(WebServerFlags::DEBUGGER);
	canAllowDebugger_ = !WebServerStopping(WebServerFlags::DEBUGGER);

	// For the UI tab's notification tests.
	if (pretendIngame_) {
		g_OSD.NudgeIngameNotifications();
	}
}

void DeveloperToolsScreen::MemoryMapTest() {
	int sum = 0;
	for (uint64_t addr = 0; addr < 0x100000000ULL; addr += 0x1000) {
		const u32 addr32 = (u32)addr;
		if (Memory::IsValidAddress(addr32)) {
			sum += Memory::ReadUnchecked_U32(addr32);
		}
	}
	// Just to force the compiler to do things properly.
	INFO_LOG(Log::JIT, "Total sum: %08x", sum);
}

static bool RunMemstickTest(std::string * error) {
	Path testRoot = GetSysDirectory(PSPDirectories::DIRECTORY_CACHE) / "test";

	*error = "N/A";

	File::CreateDir(testRoot);
	if (!File::Exists(testRoot)) {
		return false;
	}

	Path testFilePath = testRoot / "temp.txt";
	File::CreateEmptyFile(testFilePath);

	// Attempt to delete the test root. This should fail since it still contains files.
	File::DeleteDir(testRoot);
	if (!File::Exists(testRoot)) {
		*error = "testroot was deleted with a file in it!";
		return false;
	}

	File::Delete(testFilePath);
	if (File::Exists(testFilePath)) {
		*error = "testfile wasn't deleted";
		return false;
	}

	File::DeleteDir(testRoot);
	if (File::Exists(testRoot)) {
		*error = "testroot wasn't deleted, even when empty";
		return false;
	}

	*error = "passed";
	return true;
}

void DeveloperToolsScreen::OnMemstickTest(UI::EventParams & e) {
	std::string error;
	if (RunMemstickTest(&error)) {
		g_OSD.Show(OSDType::MESSAGE_SUCCESS, "Memstick test passed", error, 6.0f);
	}
	else {
		g_OSD.Show(OSDType::MESSAGE_ERROR, "Memstick test failed", error, 6.0f);
	}
}
