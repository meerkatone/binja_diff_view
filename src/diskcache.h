#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace bn_graph_diff {

inline uint64_t diskCacheBudgetBytes()
{
	bool valid = false;
	int mib = qEnvironmentVariableIntValue("BINJA_DIFF_DISK_CACHE_MB", &valid);
	return static_cast<uint64_t>(valid ? std::clamp(mib, 0, 1048576) : 4096) * 1024 * 1024;
}

// Root of every persistent artifact: extractions, matches, and corpus manifests.
// Empty when no writable cache location exists.
inline QString diskCacheRoot()
{
	QString root = qEnvironmentVariable("BINJA_DIFF_CACHE_ROOT");
	if (root.isEmpty())
		root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
	return root;
}

struct DiskCacheRetention
{
	uint64_t bytes = 0;
	size_t entries = 0;
	size_t removed = 0;
};

// Best-effort retention of generated artifacts only. Manifests, exports,
// temporary files, directories, and symbolic links are never candidates.
inline DiskCacheRetention trimDiskCache(const QString& root, uint64_t budget,
	size_t maximumEntries = 512)
{
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);
	struct Artifact { QString path; uint64_t bytes; qint64 modified; };
	std::vector<Artifact> artifacts;
	DiskCacheRetention result;
	if (QFileInfo(root).isSymLink()) return result;
	for (const auto& kind : {QString("matches"), QString("extractions")})
	{
		QDir directory(QDir(root).filePath(kind));
		if (QFileInfo(directory.absolutePath()).isSymLink()) continue;
		QRegularExpression name(kind == "matches"
		    ? "^[0-9a-f]{64}\\.json$" : "^[0-9a-f]{64}\\.cache$");
		for (const auto& file : directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::NoSort))
		{
			if (!name.match(file.fileName()).hasMatch() || file.size() < 0) continue;
			artifacts.push_back({file.absoluteFilePath(), static_cast<uint64_t>(file.size()),
			    file.lastModified().toMSecsSinceEpoch()});
			result.bytes += static_cast<uint64_t>(file.size());
		}
	}
	result.entries = artifacts.size();
	std::sort(artifacts.begin(), artifacts.end(), [budget](const auto& a, const auto& b) {
		// A legacy oversized entry must not force out every usable smaller one.
		if ((a.bytes > budget) != (b.bytes > budget)) return a.bytes > budget;
		return a.modified != b.modified ? a.modified < b.modified : a.path < b.path;
	});
	for (const auto& artifact : artifacts)
	{
		if (result.bytes <= budget && result.entries <= maximumEntries) break;
		if (QFile::remove(artifact.path))
		{
			result.bytes -= artifact.bytes;
			--result.entries;
			++result.removed;
		}
	}
	return result;
}

inline void trimDiskCacheForArtifact(const QString& path)
{
	QDir root = QFileInfo(path).absoluteDir();
	if (root.cdUp()) trimDiskCache(root.absolutePath(), diskCacheBudgetBytes());
}

inline void touchDiskCacheArtifact(QFile& file)
{
	file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
}

} // namespace bn_graph_diff
