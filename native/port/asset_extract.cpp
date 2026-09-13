/* First-run extraction of Rack's system assets from the APK.
 *
 * AAssetDir cannot enumerate subdirectories, so the Gradle build packs
 * Rack's res/ tree + Core.json + template.vcv into a single assets/system.zip
 * (see app/build.gradle.kts, task packSystemAssets). At startup we unpack it
 * into the app's files dir with libarchive, which is already linked for
 * .tar.zst patch support.
 */
#include "asset_extract.hpp"

#include <cstdio>
#include <vector>

#include <android/asset_manager.h>
#include <android/log.h>

#include <archive.h>
#include <archive_entry.h>

#include <system.hpp>
#include <common.hpp>
#include <logger.hpp>


namespace rackdroid {


static const char* MARKER_NAME = "/.assets-version";
/* Bump when the system.zip contents change without a Rack version change
   (e.g. new directories added to the Gradle packSystemAssets task). */
static const char* ASSETS_REVISION = "r28"; // r28: translations rebranded to RackDroid (packSystemAssets rewrites them); r27: themed rack graphics (themes/<name>/); r24: 909/707/606/505 drums; r23: Geomini app typeface; r22: panel/rail contrast fix; r21: glass panels (sheen+translucency); r20: warm-studio palette; r19: modern regenerated art (gradients); r18: RackDroid Drums (first-party, original); r17: Befaco(regen art) + NLC; r16: demo audio driver fix; r15: demo seed refresh; r14: Aria + touch tutorial + demo patches; r13: AudibleInstruments (regen art); r12: Autinn + FrozenWasteland(regen art); r11: 5 new plugin packs; r10: real slider art; r9: mm-unit fix on regenerated SVGs; r8: HetrickCV res; r7: Valley plugin res; r6: perforated grate rail background; r5: original (non-VCV) graphics

static const char* THUMBS_MARKER_NAME = "/.thumbs-version";
/* Bump when graphics/browser-thumbs/ is regenerated (rack_ui_smoke
   --export-thumbnails). Independent of ASSETS_REVISION above. */
static const char* THUMBS_REVISION = "t19"; // t19: only bundled plugins (Core/Fundamental/RackDroidDrums) ship here now -- the rest travel inside their own .rdmod (see app/build.gradle.kts packThumbnailAssets, scripts/make_rdmods.sh); t18: WebP thumbnails (73->17MB); t17: 909/707/606/505 drums; t16: Geomini labels; t15: panel/rail contrast fix; t14: glass panels; t13: warm-studio palette; t12: modern art restyle; t11: RackDroid Drums thumbnails


/** Shared extraction loop: streams a zip asset (via libarchive) into
`destDir`, gated by a version marker file so repeat launches skip the work. */
static bool extractZipAsset(AAssetManager* am, const char* assetName,
		const std::string& destDir, const char* markerName, const std::string& version) {
	std::string marker = destDir + markerName;

	if (rack::system::isFile(marker)) {
		if (rack::system::readFile(marker) == std::vector<uint8_t>(version.begin(), version.end()))
			return true; // Already extracted for this version
	}

	AAsset* asset = AAssetManager_open(am, assetName, AASSET_MODE_STREAMING);
	if (!asset) {
		__android_log_print(ANDROID_LOG_ERROR, "rackdroid", "assets/%s missing from APK", assetName);
		WARN("assets/%s missing from APK", assetName);
		return false;
	}

	// libarchive read callbacks over the AAsset stream
	struct AssetStream {
		AAsset* asset;
		char buffer[64 * 1024];
	};
	AssetStream stream {asset, {}};

	archive* a = archive_read_new();
	archive_read_support_format_zip(a);
	archive_read_set_read_callback(a, [](archive*, void* client, const void** buff) -> la_ssize_t {
		AssetStream* s = (AssetStream*) client;
		*buff = s->buffer;
		return AAsset_read(s->asset, s->buffer, sizeof(s->buffer));
	});
	archive_read_set_callback_data(a, &stream);

	bool ok = true;
	if (archive_read_open1(a) != ARCHIVE_OK) {
		__android_log_print(ANDROID_LOG_ERROR, "rackdroid", "%s: %s", assetName, archive_error_string(a));
		WARN("%s: %s", assetName, archive_error_string(a));
		ok = false;
	}

	archive_entry* entry;
	while (ok && archive_read_next_header(a, &entry) == ARCHIVE_OK) {
		std::string path = destDir + "/" + archive_entry_pathname(entry);
		if (archive_entry_filetype(entry) == AE_IFDIR) {
			rack::system::createDirectories(path);
			continue;
		}
		rack::system::createDirectories(rack::system::getDirectory(path));
		FILE* f = std::fopen(path.c_str(), "wb");
		if (!f) {
			__android_log_print(ANDROID_LOG_ERROR, "rackdroid", "cannot write %s", path.c_str());
			WARN("cannot write %s", path.c_str());
			ok = false;
			break;
		}
		const void* buff;
		size_t size;
		la_int64_t offset;
		int r;
		while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
			if (std::fwrite(buff, 1, size, f) != size) {
				ok = false;
				break;
			}
		}
		if (r != ARCHIVE_EOF && r != ARCHIVE_OK)
			ok = false;
		std::fclose(f);
	}

	archive_read_free(a);
	AAsset_close(asset);

	if (ok) {
		rack::system::writeFile(marker, std::vector<uint8_t>(version.begin(), version.end()));
		__android_log_print(ANDROID_LOG_INFO, "rackdroid", "%s extracted to %s", assetName, destDir.c_str());
	}
	return ok;
}


bool extractSystemAssets(AAssetManager* am, const std::string& systemDir) {
	return extractZipAsset(am, "system.zip", systemDir, MARKER_NAME,
		rack::APP_VERSION + "+" + ASSETS_REVISION);
}


bool extractThumbnailAssets(AAssetManager* am, const std::string& thumbsDir) {
	return extractZipAsset(am, "thumbnails.zip", thumbsDir, THUMBS_MARKER_NAME,
		std::string(THUMBS_REVISION));
}


void applyRackTheme(const std::string& systemDir, const std::string& userDir) {
	// The rack graphics (background/rail + module panels) come in themed
	// variants under systemDir/themes/<name>/, each mirroring the canonical
	// res/ + plugins/*/res/ layout. Kotlin (AppTheme) persists the chosen
	// theme in userDir/rack-theme.txt. Here -- at startup, BEFORE any panel
	// SVG is loaded (Rack's Svg::load caches by filename for the process
	// lifetime, so this must run first) -- we copy the chosen theme's files
	// over the canonical paths.
	//
	// Skipped when the same theme is already in place over the same assets.
	// It used to run unconditionally, which sounds cheap for ~250 tiny SVGs
	// and measured ~51 ms on an S22 -- but it is also 256 deletes and 256
	// writes to flash on every single launch, to end up byte for byte where
	// the previous launch left off. The marker carries the asset revision as
	// well as the theme name, so extractSystemAssets() laying down fresh
	// canonical files (a new APP_VERSION or ASSETS_REVISION) still forces the
	// theme back over them -- that ordering is why this runs after it.
	std::string themeFile = userDir + "/rack-theme.txt";
	std::string theme = "amber";
	if (rack::system::isFile(themeFile)) {
		std::vector<uint8_t> b = rack::system::readFile(themeFile);
		theme.assign(b.begin(), b.end());
		while (!theme.empty() && (theme.back() == '\n' || theme.back() == '\r'
				|| theme.back() == ' ' || theme.back() == '\t'))
			theme.pop_back();
	}
	std::string marker = userDir + "/.rack-theme-applied";
	std::string stamp = theme + "+" + rack::APP_VERSION + "+" + ASSETS_REVISION;
	if (rack::system::isFile(marker)) {
		std::vector<uint8_t> have = rack::system::readFile(marker);
		if (std::string(have.begin(), have.end()) == stamp) {
			__android_log_print(ANDROID_LOG_INFO, "rackdroid",
				"rack theme '%s' already in place", theme.c_str());
			return;
		}
	}

	std::string themeDir = systemDir + "/themes/" + theme;
	if (!rack::system::isDirectory(themeDir)) {
		__android_log_print(ANDROID_LOG_INFO, "rackdroid",
			"rack theme '%s' not bundled; using canonical graphics", theme.c_str());
		return;
	}
	int n = 0;
	for (const std::string& src : rack::system::getEntries(themeDir, -1)) {
		if (rack::system::isDirectory(src))
			continue;
		std::string rel = src.substr(themeDir.size()); // leading '/'
		std::string dst = systemDir + rel;
		rack::system::createDirectories(rack::system::getDirectory(dst));
		rack::system::remove(dst); // system::copy won't overwrite an existing file
		if (rack::system::copy(src, dst))
			n++;
	}
	// Only after the copies succeeded: a half-applied theme must be retried on
	// the next launch, not remembered as done.
	if (FILE* f = std::fopen(marker.c_str(), "w")) {
		std::fwrite(stamp.data(), 1, stamp.size(), f);
		std::fclose(f);
	}
	__android_log_print(ANDROID_LOG_INFO, "rackdroid",
		"applied rack theme '%s' (%d files)", theme.c_str(), n);
}


void seedDemoPatches(const std::string& systemDir, const std::string& userDir) {
	// The bundled demo .vcv files (graphics/template-android/patches, packed
	// into system.zip) are copied into the user patches dir so File > Open
	// lists them immediately. Within one app version, missing files are not
	// re-copied (deleting a demo sticks); on an assets-revision bump the
	// demos are refreshed so fixes reach existing installs.
	std::string src = systemDir + "/patches";
	std::string dst = userDir + "/patches";
	if (!rack::system::isDirectory(src))
		return;
	rack::system::createDirectories(dst);
	std::string marker = dst + "/.seeded-revision";
	std::string version = rack::APP_VERSION + "+" + ASSETS_REVISION;
	bool refresh = true;
	if (rack::system::isFile(marker))
		refresh = rack::system::readFile(marker) != std::vector<uint8_t>(version.begin(), version.end());
	for (const std::string& entry : rack::system::getEntries(src)) {
		if (rack::system::getExtension(entry) != ".vcv")
			continue;
		std::string target = dst + "/" + rack::system::getFilename(entry);
		if (rack::system::exists(target) && !refresh)
			continue;
		rack::system::copy(entry, target);
		__android_log_print(ANDROID_LOG_INFO, "rackdroid", "seeded demo patch %s", target.c_str());
	}
	rack::system::writeFile(marker, std::vector<uint8_t>(version.begin(), version.end()));
}


} // namespace rackdroid
