#pragma once

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <optional>

namespace bn_graph_diff {

// Hash the saved database itself so a replacement that preserves size and
// modification time cannot reuse a prior extraction or match result.
inline std::optional<QByteArray> sha256File(const QFileInfo& info)
{
	const qint64 expectedSize = info.size();
	const QDateTime expectedMtime = info.lastModified();
	const QDateTime expectedChangeTime = info.metadataChangeTime();
	QFile file(info.absoluteFilePath());
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
	QFileInfo current(info.absoluteFilePath());
	if (file.error() != QFileDevice::NoError || file.pos() != expectedSize
	    || current.size() != expectedSize || current.lastModified() != expectedMtime
	    || current.metadataChangeTime() != expectedChangeTime)
		return std::nullopt;
	return hash.result().toHex();
}

} // namespace bn_graph_diff
