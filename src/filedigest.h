#pragma once

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace bn_graph_diff {

// Identity of the file object plus its last metadata change. Unlike the
// modification time, neither part can be restored by ordinary file APIs after a
// rewrite. POSIX: device, inode, and nanosecond ctime. Windows: volume serial,
// file index, and NTFS ChangeTime, which QFileInfo::metadataChangeTime does not
// expose there (it mirrors the modification time).
using FileChangeStamp = std::tuple<uint64_t, uint64_t, int64_t>;

inline std::optional<FileChangeStamp> fileChangeStamp(const QString& path)
{
#if defined(_WIN32)
	const QString native = QDir::toNativeSeparators(path);
	HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
	    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return std::nullopt;
	BY_HANDLE_FILE_INFORMATION information {};
	FILE_BASIC_INFO basic {};
	const bool valid = GetFileInformationByHandle(handle, &information)
	    && GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
	CloseHandle(handle);
	if (!valid)
		return std::nullopt;
	return FileChangeStamp {information.dwVolumeSerialNumber,
	    (static_cast<uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow,
	    basic.ChangeTime.QuadPart};
#else
	struct stat status {};
	if (::stat(QFile::encodeName(path).constData(), &status) != 0)
		return std::nullopt;
#if defined(__APPLE__)
	const auto& changed = status.st_ctimespec;
#else
	const auto& changed = status.st_ctim;
#endif
	return FileChangeStamp {static_cast<uint64_t>(status.st_dev),
	    static_cast<uint64_t>(status.st_ino),
	    static_cast<int64_t>(changed.tv_sec) * 1000000000 + changed.tv_nsec};
#endif
}

// Hash the saved database itself so a replacement that preserves size and
// modification time cannot reuse a prior extraction or match result.
inline std::optional<QByteArray> sha256File(const QFileInfo& info)
{
	const QString path = info.absoluteFilePath();
	const qint64 expectedSize = info.size();
	const QDateTime expectedMtime = info.lastModified();
	const auto expectedChange = fileChangeStamp(path);
	if (!expectedChange)
		return std::nullopt;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() != expectedSize)
		return std::nullopt;
	QCryptographicHash hash(QCryptographicHash::Sha256);
	QByteArray buffer(1024 * 1024, Qt::Uninitialized);
	while (true)
	{
		const qint64 count = file.read(buffer.data(), buffer.size());
		if (count < 0)
			return std::nullopt;
		if (count == 0)
			break;
		hash.addData(QByteArrayView(buffer.constData(), count));
	}
	QFileInfo current(path);
	if (file.error() != QFileDevice::NoError || file.pos() != expectedSize
	    || current.size() != expectedSize || current.lastModified() != expectedMtime
	    || fileChangeStamp(path) != expectedChange)
		return std::nullopt;
	return hash.result().toHex();
}

// Reuse a digest while the file's path, size, modification time, and change
// stamp are unchanged. Entries are scoped to an owner (a Binary Ninja file
// session), so a newly opened file is always hashed again.
class FileDigestMemo
{
  public:
	using Hasher = std::function<std::optional<QByteArray>(const QFileInfo&)>;

	explicit FileDigestMemo(Hasher hasher = sha256File) : m_hasher(std::move(hasher)) {}

	std::optional<QByteArray> digest(size_t owner, const QFileInfo& info)
	{
		const QString path = info.absoluteFilePath();
		auto stamp = currentStamp(path);
		if (stamp)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			auto found = m_entries.find(owner);
			if (found != m_entries.end() && found->second.first == *stamp)
				return found->second.second;
		}
		// Hash without the lock; concurrent misses for one file may duplicate work.
		auto digest = m_hasher(QFileInfo(path));
		if (!digest)
			return std::nullopt;
		// Remember the digest only if the file did not change around the hash.
		if (stamp && currentStamp(path) == stamp)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_entries.size() >= kMaximumEntries && !m_entries.count(owner))
				m_entries.clear();
			m_entries[owner] = {std::move(*stamp), *digest};
		}
		return digest;
	}

  private:
	using Stamp = std::tuple<QString, qint64, qint64, FileChangeStamp>;
	static constexpr size_t kMaximumEntries = 1024;

	static std::optional<Stamp> currentStamp(const QString& path)
	{
		const QFileInfo info(path);
		auto change = fileChangeStamp(path);
		if (!info.exists() || !change)
			return std::nullopt;
		return Stamp {path, info.size(), info.lastModified().toMSecsSinceEpoch(), *change};
	}

	Hasher m_hasher;
	std::mutex m_mutex;
	std::map<size_t, std::pair<Stamp, QByteArray>> m_entries;
};

} // namespace bn_graph_diff
